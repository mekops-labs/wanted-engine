/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-fb.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* Framebuffer device driver: /dev/fb/<screen>/{info,data,ctl}, one subtree per
 * granted screen. Screens live in an engine-wide table so a writer and its
 * observers, which are different wapps, reach the same pixels. */

static const char id[] = {'F', 'b', 'D', 'v'};

#define FB_MAX_FDS 8
#define FB_OBSERVE_TOKEN "observe"
#define FB_SCREENS_KEY "screens="

#define FB_NODE_ROOT 0
#define FB_NODE_SCREEN 1
#define FB_NODE_INFO 2
#define FB_NODE_DATA 3
#define FB_NODE_CTL 4
#define FB_NODE_DAMAGE 5

#define FB_CTL_MAX 48
#define FB_CTL_FIELDS 6
#define FB_COORD_MAX 65535

#define FB_INFO_FMT "%u %u %s %u\n"
#define FB_INFO_MAX 32

#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
#define FB_DAMAGE_REC 8
#define FB_SLEEP_NS 1000000ULL /* 1 ms */

typedef struct fb_damage_t {
    uint16_t x;
    uint16_t y;
    uint16_t w;
    uint16_t h;
} fb_damage_t;

/* Ring of flushed rectangles one observer has not read yet. */
typedef struct fb_observer_t {
    fb_damage_t q[CONFIG_WANTED_FB_DAMAGE_QUEUE];
    uint16_t head;
    uint16_t count;
} fb_observer_t;
#endif

typedef struct fb_screen_t {
    char name[FB_NAME_MAX + 1];
    uint16_t width;
    uint16_t height;
    uint8_t format;
    uint32_t stride;
    uint32_t size;
    uint8_t *pixels;
    const fb_backing_ops_t *ops;
    void *backing;
    bool writer;
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    uint8_t *flushed; /* held while an observer exists */
    fb_observer_t *observers[CONFIG_WANTED_FB_MAX_OBSERVERS];
    uint8_t observerCnt;
#endif
} fb_screen_t;

static struct {
    fb_screen_t screens[CONFIG_WANTED_FB_MAX_SCREENS];
    uint8_t count;
    platform_mutex_t *lock;
} table;

struct fb_fd_t {
    bool used;
    uint8_t node;
    uint8_t screen;
    bool nonblock;
    uint32_t offset;
};

struct vfs_driver_ctx_t {
    fb_screen_t *screens[CONFIG_WANTED_FB_MAX_SCREENS];
    uint8_t count;
    bool observe;
    bool claimed;
    int wakeFd;
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    fb_observer_t queues[CONFIG_WANTED_FB_MAX_SCREENS];
#endif
    struct fb_fd_t fds[FB_MAX_FDS];
};

/* ── Screen table ────────────────────────────────────────────────────────── */

static uint8_t pixelBytes(uint8_t format) {
    return format == FB_FORMAT_RGB888 ? 3 : 2;
}

static const char *formatName(uint8_t format) {
    return format == FB_FORMAT_RGB888 ? "rgb888" : "rgb565";
}

/* [A-Za-z0-9_-], 1..FB_NAME_MAX characters. The observe token is reserved. */
static bool validName(const char *s) {
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
            return false;
    }
    return n > 0 && n <= FB_NAME_MAX && strcmp(s, FB_OBSERVE_TOKEN) != 0;
}

static fb_screen_t *findScreen(const char *name, size_t nameLen) {
    for (uint8_t i = 0; i < table.count; i++) {
        if (strlen(table.screens[i].name) == nameLen &&
            strncmp(table.screens[i].name, name, nameLen) == 0)
            return &table.screens[i];
    }
    return NULL;
}

int FbScreenRegister(const fb_screen_desc_t *desc) {
    if (desc == NULL || desc->name == NULL || !validName(desc->name) ||
        desc->width == 0 || desc->height == 0 ||
        (desc->format != FB_FORMAT_RGB565 && desc->format != FB_FORMAT_RGB888))
        return -EINVAL;
    const fb_screen_t *have = findScreen(desc->name, strlen(desc->name));
    if (have != NULL) {
        bool same = have->width == desc->width &&
                    have->height == desc->height &&
                    have->format == desc->format && have->ops == desc->ops &&
                    have->backing == desc->backing;
        return same ? 0 : -EEXIST;
    }
    if (table.count >= CONFIG_WANTED_FB_MAX_SCREENS)
        return -ENOSPC;

    fb_screen_t *s = &table.screens[table.count];
    memset(s, 0, sizeof(*s));
    strncpy(s->name, desc->name, FB_NAME_MAX);
    s->width = desc->width;
    s->height = desc->height;
    s->format = desc->format;
    s->stride = (uint32_t)desc->width * pixelBytes(desc->format);
    s->size = s->stride * desc->height;
    s->ops = desc->ops;
    s->backing = desc->backing;
    s->pixels = WantedMalloc(s->size);
    if (s->pixels == NULL)
        return -ENOMEM;
    memset(s->pixels, 0, s->size);

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    table.count++;
    return 0;
}

void FbScreensReset(void) {
    for (uint8_t i = 0; i < table.count; i++) {
        WantedFree(table.screens[i].pixels);
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
        WantedFree(table.screens[i].flushed);
#endif
    }
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse screens=<name>[,<name>...][,observe] and resolve every name. Returns 0
 * or -EINVAL; the caller frees the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(FB_SCREENS_KEY) - 1;

    if (options == NULL || strncmp(options, FB_SCREENS_KEY, keyLen) != 0) {
        DEBUG_TRACE("fb grant has no screens= clause");
        return -EINVAL;
    }

    const char *p = options + keyLen;
    if (*p == '\0')
        return -EINVAL;

    for (;;) {
        const char *end = p;
        while (*end != '\0' && *end != ',')
            end++;
        size_t len = (size_t)(end - p);

        if (len == 0)
            return -EINVAL;
        if (len == sizeof(FB_OBSERVE_TOKEN) - 1 &&
            strncmp(p, FB_OBSERVE_TOKEN, len) == 0) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
            ctx->observe = true;
#else
            return -EINVAL;
#endif
        } else {
            fb_screen_t *s = findScreen(p, len);
            if (s == NULL || ctx->count >= CONFIG_WANTED_FB_MAX_SCREENS) {
                DEBUG_TRACE("fb grant names an unknown screen");
                return -EINVAL;
            }
            for (uint8_t i = 0; i < ctx->count; i++) {
                if (ctx->screens[i] == s)
                    return -EINVAL;
            }
            ctx->screens[ctx->count++] = s;
        }

        if (*end == '\0')
            break;
        p = end + 1;
    }
    return ctx->count > 0 ? 0 : -EINVAL;
}

/* Take every granted screen for this writer, or none of them. */
static int claimScreens(struct vfs_driver_ctx_t *ctx) {
    int rc = 0;

    PlatformMutexLock(table.lock);
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (ctx->screens[i]->writer)
            rc = -EBUSY;
    }
    if (rc == 0) {
        for (uint8_t i = 0; i < ctx->count; i++)
            ctx->screens[i]->writer = true;
        ctx->claimed = true;
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
static void detachObserver(fb_screen_t *s, const fb_observer_t *q) {
    for (uint8_t i = 0; i < s->observerCnt; i++) {
        if (s->observers[i] == q) {
            s->observers[i] = s->observers[--s->observerCnt];
            break;
        }
    }
    if (s->observerCnt == 0) {
        WantedFree(s->flushed);
        s->flushed = NULL;
    }
}

/* Register this grant's queues on every screen, or on none. The first observer
 * of a screen allocates its flushed copy, starting from the live pixels so a
 * late observer sees the screen. */
static int attachObservers(struct vfs_driver_ctx_t *ctx) {
    int rc = 0;
    uint8_t done = 0;

    PlatformMutexLock(table.lock);
    for (; done < ctx->count && rc == 0; done++) {
        fb_screen_t *s = ctx->screens[done];
        if (s->observerCnt >= CONFIG_WANTED_FB_MAX_OBSERVERS) {
            rc = -EBUSY;
        } else if (s->flushed == NULL) {
            s->flushed = WantedMalloc(s->size);
            if (s->flushed == NULL)
                rc = -ENOMEM;
            else
                memcpy(s->flushed, s->pixels, s->size);
        }
        if (rc == 0)
            s->observers[s->observerCnt++] = &ctx->queues[done];
    }
    if (rc < 0) {
        for (uint8_t i = 0; i + 1 < done; i++)
            detachObserver(ctx->screens[i], &ctx->queues[i]);
    } else {
        ctx->claimed = true;
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}
#endif

static void releaseScreens(struct vfs_driver_ctx_t *ctx) {
    if (!ctx->claimed)
        return;
    PlatformMutexLock(table.lock);
    for (uint8_t i = 0; i < ctx->count; i++) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
        if (ctx->observe)
            detachObserver(ctx->screens[i], &ctx->queues[i]);
        else
#endif
            ctx->screens[i]->writer = false;
    }
    PlatformMutexUnlock(table.lock);
}

static int acquireScreens(struct vfs_driver_ctx_t *ctx) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    if (ctx->observe)
        return attachObservers(ctx);
#endif
    return claimScreens(ctx);
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *screen) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = FB_NODE_ROOT;
        *screen = 0;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;

    int idx = -1;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strlen(ctx->screens[i]->name) == (size_t)(slash - p) &&
            strncmp(ctx->screens[i]->name, p, (size_t)(slash - p)) == 0)
            idx = i;
    }
    if (idx < 0)
        return -ENOENT;
    *screen = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0') {
        *node = FB_NODE_SCREEN;
        return 0;
    }
    if (strcmp(slash, "info") == 0)
        *node = FB_NODE_INFO;
    else if (strcmp(slash, "data") == 0)
        *node = FB_NODE_DATA;
    else if (strcmp(slash, "ctl") == 0 && !ctx->observe)
        *node = FB_NODE_CTL;
    else if (strcmp(slash, "damage") == 0 && ctx->observe)
        *node = FB_NODE_DAMAGE;
    else
        return -ENOENT;
    return 0;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);
static void _SetWake(vfs_driver_ctx_t d, int fd);
static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags);
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsFbInit(const wapp_t *wapp, const char *options) {
    (void)wapp;

    vfs_driver_t *driver = WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }
    struct vfs_driver_ctx_t *ctx = WantedMalloc(sizeof(*ctx));
    if (ctx == NULL) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->wakeFd = -1;
    memset(driver, 0, sizeof(*driver));

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Seek = _Seek;
    driver->SetWake = _SetWake;
    driver->SetFlags = _SetFlags;
    driver->Poll = _Poll;
    driver->ReadDir = _ReadDir;

    if (parseGrant(ctx, options) < 0 || acquireScreens(ctx) < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d->ctx != NULL)
        releaseScreens(d->ctx);
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct fb_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= FB_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    uint8_t screen;
    int rc = resolve(d, path != NULL ? path : "", &node, &screen);
    if (rc < 0)
        return rc;

    for (int i = 0; i < FB_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            d->fds[i].screen = screen;
            d->fds[i].nonblock = (flags & VFS_O_NONBLOCK) != 0;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == FB_NODE_ROOT || f->node == FB_NODE_SCREEN;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    if (f->node == FB_NODE_DATA)
        s->size = d->screens[f->screen]->size;
    return 0;
}

static int readInfo(vfs_driver_ctx_t d, struct fb_fd_t *f, void *buf,
                    size_t nbyte) {
    const fb_screen_t *s = d->screens[f->screen];
    char line[FB_INFO_MAX];
    int len = snprintf(line, sizeof(line), FB_INFO_FMT, (unsigned)s->width,
                       (unsigned)s->height, formatName(s->format),
                       (unsigned)s->stride);
    if (len < 0 || f->offset >= (uint32_t)len)
        return 0;

    size_t n = (size_t)len - f->offset;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, line + f->offset, n);
    f->offset += (uint32_t)n;
    return (int)n;
}

/* What a read of data shows: the flushed copy for an observer. */
static const uint8_t *dataSource(vfs_driver_ctx_t d, const fb_screen_t *s) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    if (d->observe)
        return s->flushed;
#endif
    (void)d;
    return s->pixels;
}

/* Move up to `nbyte` bytes between `buf` and the pixels at the fd's offset,
 * truncated at the end of the screen. */
static int transferData(vfs_driver_ctx_t d, struct fb_fd_t *f, void *buf,
                        size_t nbyte, bool write) {
    const fb_screen_t *s = d->screens[f->screen];
    if (f->offset >= s->size)
        return 0;

    size_t n = s->size - f->offset;
    if (n > nbyte)
        n = nbyte;

    PlatformMutexLock(table.lock);
    if (write)
        memcpy(s->pixels + f->offset, buf, n);
    else
        memcpy(buf, dataSource(d, s) + f->offset, n);
    PlatformMutexUnlock(table.lock);

    f->offset += (uint32_t)n;
    return (int)n;
}

static void _SetWake(vfs_driver_ctx_t d, int fd) { d->wakeFd = fd; }

static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags) {
    struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    f->nonblock = (flags & VFS_O_NONBLOCK) != 0;
    return 0;
}

/* Only damage waits on anything; every other node is always ready. */
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail) {
    const struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    if (f->node == FB_NODE_DAMAGE) {
        PlatformMutexLock(table.lock);
        uint16_t count = d->queues[f->screen].count;
        PlatformMutexUnlock(table.lock);
        *avail = (uint32_t)count * FB_DAMAGE_REC;
        return count > 0 ? VFS_POLL_IN : 0;
    }
#endif
    return VFS_POLL_IN | VFS_POLL_OUT;
}

/* Decimal digits only, at most FB_COORD_MAX. */
static bool parseCoord(const char *s, uint16_t *out) {
    uint32_t v = 0;
    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = v * 10 + (uint32_t)(*s - '0');
        if (v > FB_COORD_MAX)
            return false;
    }
    *out = (uint16_t)v;
    return true;
}

#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
static void queueDamage(fb_observer_t *o, const fb_damage_t *rec,
                        const fb_screen_t *s) {
    if (o->count >= CONFIG_WANTED_FB_DAMAGE_QUEUE) {
        o->head = 0;
        o->count = 1;
        o->q[0] = (fb_damage_t){0, 0, s->width, s->height};
        return;
    }
    o->q[(o->head + o->count) % CONFIG_WANTED_FB_DAMAGE_QUEUE] = *rec;
    o->count++;
}

/* Copy the rectangle into the flushed copy, then tell every observer. */
static void publishFlush(fb_screen_t *s, uint16_t x, uint16_t y, uint16_t w,
                         uint16_t h) {
    const uint8_t bpp = pixelBytes(s->format);
    const fb_damage_t rec = {x, y, w, h};

    PlatformMutexLock(table.lock);
    if (s->flushed != NULL) {
        for (uint16_t row = y; row < y + h; row++) {
            size_t off = (size_t)row * s->stride + (size_t)x * bpp;
            memcpy(s->flushed + off, s->pixels + off, (size_t)w * bpp);
        }
        for (uint8_t i = 0; i < s->observerCnt; i++)
            queueDamage(s->observers[i], &rec, s);
    }
    PlatformMutexUnlock(table.lock);
}
#endif

static int flushRect(vfs_driver_ctx_t d, fb_screen_t *s, uint16_t x, uint16_t y,
                     uint16_t w, uint16_t h) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    publishFlush(s, x, y, w, h);
#endif
    if (s->ops == NULL || s->ops->Flush == NULL)
        return 0;
    return s->ops->Flush(s->backing, x, y, w, h, d->wakeFd);
}

/* One ctl line: "flush", "flush <x> <y> <w> <h>", "blank on" or "blank off". */
static int runCtl(vfs_driver_ctx_t d, fb_screen_t *s, char *line) {
    char *tok[FB_CTL_FIELDS];
    size_t n = 0;
    for (char *p = line; *p != '\0';) {
        while (*p == ' ')
            *p++ = '\0';
        if (*p == '\0')
            break;
        if (n == FB_CTL_FIELDS)
            return -EINVAL;
        tok[n++] = p;
        while (*p != '\0' && *p != ' ')
            p++;
    }
    if (n == 0)
        return -EINVAL;

    if (strcmp(tok[0], "blank") == 0) {
        bool on = n == 2 && strcmp(tok[1], "on") == 0;
        if (!on && !(n == 2 && strcmp(tok[1], "off") == 0))
            return -EINVAL;
        if (s->ops == NULL || s->ops->Blank == NULL)
            return 0;
        return s->ops->Blank(s->backing, on);
    }
    if (strcmp(tok[0], "flush") != 0)
        return -EINVAL;
    if (n == 1)
        return flushRect(d, s, 0, 0, s->width, s->height);
    if (n != 5)
        return -EINVAL;

    uint16_t r[4];
    for (size_t i = 0; i < 4; i++) {
        if (!parseCoord(tok[i + 1], &r[i]))
            return -EINVAL;
    }
    if (r[2] == 0 || r[3] == 0 || (uint32_t)r[0] + r[2] > s->width ||
        (uint32_t)r[1] + r[3] > s->height)
        return -EINVAL;
    return flushRect(d, s, r[0], r[1], r[2], r[3]);
}

static int writeCtl(vfs_driver_ctx_t d, struct fb_fd_t *f, const void *buf,
                    size_t nbyte) {
    char line[FB_CTL_MAX];
    size_t len = nbyte;
    if (len > 0 && ((const char *)buf)[len - 1] == '\n')
        len--;
    if (len >= sizeof(line))
        return -EINVAL;
    memcpy(line, buf, len);
    line[len] = '\0';

    int rc = runCtl(d, d->screens[f->screen], line);
    return rc < 0 ? rc : (int)nbyte;
}

#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
static void putLe16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* Pop whole records into `buf`. Returns bytes copied, 0 when none queued. */
static int popDamage(fb_observer_t *o, uint8_t *buf, size_t nbyte) {
    int used = 0;

    PlatformMutexLock(table.lock);
    while (o->count > 0 && (size_t)used + FB_DAMAGE_REC <= nbyte) {
        const fb_damage_t *r = &o->q[o->head];
        putLe16(buf + used, r->x);
        putLe16(buf + used + 2, r->y);
        putLe16(buf + used + 4, r->w);
        putLe16(buf + used + 6, r->h);
        o->head = (uint16_t)((o->head + 1) % CONFIG_WANTED_FB_DAMAGE_QUEUE);
        o->count--;
        used += FB_DAMAGE_REC;
    }
    PlatformMutexUnlock(table.lock);
    return used;
}

/* Blocks until a flush is queued; a stop ends the wait. */
static int readDamage(vfs_driver_ctx_t d, struct fb_fd_t *f, void *buf,
                      size_t nbyte) {
    if (nbyte < FB_DAMAGE_REC)
        return -EINVAL;

    for (;;) {
        int n = popDamage(&d->queues[f->screen], buf, nbyte);
        if (n > 0)
            return n;
        if (f->nonblock)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, FB_SLEEP_NS, 0) ==
                -EINTR ||
            PlatformWakeRaised(d->wakeFd))
            return -EINTR;
    }
}
#endif

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case FB_NODE_INFO:
        return readInfo(d, f, buf, nbyte);
    case FB_NODE_DATA:
        return transferData(d, f, buf, nbyte, false);
    case FB_NODE_ROOT:
    case FB_NODE_SCREEN:
        return -EISDIR;
    case FB_NODE_CTL:
        return -EPERM;
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    case FB_NODE_DAMAGE:
        return readDamage(d, f, buf, nbyte);
#endif
    default:
        return -ENOSYS;
    }
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case FB_NODE_ROOT:
    case FB_NODE_SCREEN:
        return -EISDIR;
    case FB_NODE_INFO:
        return -EPERM;
    case FB_NODE_DATA:
        if (d->observe)
            return -EPERM;
        return transferData(d, f, (void *)buf, nbyte, true);
    case FB_NODE_CTL:
        return writeCtl(d, f, buf, nbyte);
    case FB_NODE_DAMAGE:
        return -EPERM;
    default:
        return -ENOSYS;
    }
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node != FB_NODE_DATA)
        return -ESPIPE;

    long base = 0;
    if (whence == VFS_SEEK_CUR)
        base = (long)f->offset;
    else if (whence == VFS_SEEK_END)
        base = (long)d->screens[f->screen]->size;
    else if (whence != VFS_SEEK_SET)
        return -EINVAL;

    if (off < -base || (uint64_t)(base + off) > UINT32_MAX)
        return -EINVAL;
    f->offset = (uint32_t)(base + off);
    *pos = base + off;
    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct fb_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == FB_NODE_SCREEN) {
        vfs_dir_entry_t attrs[3] = {
            {"info", VFS_FILETYPE_CHARACTER_DEVICE},
            {"data", VFS_FILETYPE_CHARACTER_DEVICE},
            {d->observe ? "damage" : "ctl", VFS_FILETYPE_CHARACTER_DEVICE},
        };
        return VfsFlatDirReadDir(attrs, 3, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != FB_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_FB_MAX_SCREENS];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->screens[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
