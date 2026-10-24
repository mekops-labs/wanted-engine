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
#include <vfs-haptic.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* Haptic driver: /dev/haptic/<name>/{ctl,effects,busy}, one subtree per
 * motor. Motors live in an engine-wide table, so a board device outlives the
 * grant that plays it. */

static const char id[] = {'H', 'a', 'P', 't'};

#define HAPTIC_MAX_FDS 8
#define HAPTIC_MOTORS_KEY "motors="
#define HAPTIC_OBSERVE_TOKEN "observe"
#define HAPTIC_MODE_ONOFF "onoff"

#define HAPTIC_NODE_ROOT 0
#define HAPTIC_NODE_DEVICE 1
#define HAPTIC_NODE_CTL 2
#define HAPTIC_NODE_EFFECTS 3
#define HAPTIC_NODE_BUSY 4

#define HAPTIC_LINE_MAX 32
#define HAPTIC_DIGITS_MAX 9
#define HAPTIC_SEGMENTS_MAX 3

static const char *const effectNames[HAPTIC_EFFECT_COUNT] = {"click", "double",
                                                             "buzz", "alert"};

/* A pin motor's effects as alternating on and off times in ms. */
static const struct {
    uint8_t count;
    uint16_t ms[HAPTIC_SEGMENTS_MAX];
} pinPatterns[HAPTIC_EFFECT_COUNT] = {
    {1, {25}},
    {3, {25, 60, 25}},
    {1, {150}},
    {1, {500}},
};

typedef struct haptic_dev_t {
    bool used;
    bool owned;
    bool generic; /* a pin motor of a live grant, freed with it */
    char name[HAPTIC_NAME_MAX + 1];
    unsigned effects;
    haptic_ops_t ops;
    void *ctx;
    platform_motor_t *pin;
} haptic_dev_t;

static struct {
    haptic_dev_t devs[CONFIG_WANTED_HAPTIC_MAX_DEVICES];
    platform_mutex_t *lock;
} table;

struct haptic_fd_t {
    bool used;
    bool done;
    uint8_t node;
    uint8_t dev;
};

struct vfs_driver_ctx_t {
    haptic_dev_t *devs[CONFIG_WANTED_HAPTIC_MAX_DEVICES];
    uint8_t count;
    bool observe;
    struct haptic_fd_t fds[HAPTIC_MAX_FDS];
};

/* ── Pin motors ──────────────────────────────────────────────────────────── */

static int pinPlay(void *ctx, unsigned effect) {
    return PlatformMotorPattern(ctx, pinPatterns[effect].ms,
                                pinPatterns[effect].count);
}

static int pinPulse(void *ctx, unsigned ms) {
    uint16_t seg = (uint16_t)ms;
    return PlatformMotorPattern(ctx, &seg, 1);
}

static void pinStop(void *ctx) { PlatformMotorStop(ctx); }

static bool pinBusy(const void *ctx) { return PlatformMotorBusy(ctx); }

static const haptic_ops_t pinOps = {pinPlay, pinPulse, pinStop, pinBusy};

/* ── Device table ────────────────────────────────────────────────────────── */

/* [A-Za-z0-9_-], 1..HAPTIC_NAME_MAX characters. */
static bool validToken(const char *s, size_t len) {
    if (len == 0 || len > HAPTIC_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

static haptic_dev_t *findDevice(const char *name, size_t len) {
    for (size_t i = 0; i < CONFIG_WANTED_HAPTIC_MAX_DEVICES; i++) {
        haptic_dev_t *d = &table.devs[i];
        if (d->used && strlen(d->name) == len &&
            strncmp(d->name, name, len) == 0)
            return d;
    }
    return NULL;
}

static haptic_dev_t *freeSlot(void) {
    for (size_t i = 0; i < CONFIG_WANTED_HAPTIC_MAX_DEVICES; i++) {
        if (!table.devs[i].used)
            return &table.devs[i];
    }
    return NULL;
}

int HapticDeviceRegister(const haptic_device_desc_t *desc) {
    if (desc == NULL || desc->name == NULL ||
        !validToken(desc->name, strlen(desc->name)) ||
        (desc->effects & ~HAPTIC_EFFECTS_ALL) != 0 || desc->ops == NULL ||
        desc->ops->play == NULL || desc->ops->pulse == NULL ||
        desc->ops->stop == NULL || desc->ops->busy == NULL)
        return -EINVAL;

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = 0;
    haptic_dev_t *d = NULL;
    if (findDevice(desc->name, strlen(desc->name)) != NULL) {
        rc = -EEXIST;
    } else {
        d = freeSlot();
        if (d == NULL)
            rc = -ENOSPC;
    }
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
        d->used = true;
        strncpy(d->name, desc->name, HAPTIC_NAME_MAX);
        d->effects = desc->effects;
        d->ops = *desc->ops;
        d->ctx = desc->ctx;
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

void HapticDevicesReset(void) {
    for (size_t i = 0; i < CONFIG_WANTED_HAPTIC_MAX_DEVICES; i++) {
        if (table.devs[i].used && table.devs[i].generic)
            PlatformMotorClose(table.devs[i].pin);
    }
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Split `s` at the first `sep` into the text before it, which stays in `s`,
 * and the rest, which is returned. NULL when there is no separator. */
/* cppcheck-suppress constParameterPointer ; the returned pointer is written */
static char *splitAt(char *s, char sep) {
    char *p = strchr(s, sep);
    if (p == NULL)
        return NULL;
    *p = '\0';
    return p + 1;
}

/* A bare name selects a device the board registered. An observer shares it
 * without taking the writer slot. */
static int claimBoardDevice(struct vfs_driver_ctx_t *ctx, const char *name) {
    haptic_dev_t *d = findDevice(name, strlen(name));
    if (d == NULL || d->generic) {
        DEBUG_TRACE("haptic grant names an unknown device");
        return -EINVAL;
    }
    if (!ctx->observe) {
        if (d->owned)
            return -EBUSY;
        d->owned = true;
    }
    ctx->devs[ctx->count++] = d;
    return 0;
}

/* name:address:onoff opens a pin motor through the platform backing. */
static int openPinMotor(struct vfs_driver_ctx_t *ctx, const char *name,
                        char *rest) {
    const char *mode = splitAt(rest, ':');
    if (ctx->observe || mode == NULL || strchr(mode, ':') != NULL ||
        rest[0] == '\0' || strcmp(mode, HAPTIC_MODE_ONOFF) != 0)
        return -EINVAL;
    if (!validToken(name, strlen(name)))
        return -EINVAL;

    haptic_dev_t *d = findDevice(name, strlen(name));
    if (d != NULL)
        return d->owned ? -EBUSY : -EINVAL;
    if ((d = freeSlot()) == NULL)
        return -ENOSPC;

    platform_motor_t *pin = NULL;
    int rc = PlatformMotorOpen(rest, &pin);
    if (rc < 0) {
        DEBUG_TRACE("haptic %s: backing refused the address (%d)", name, rc);
        return rc;
    }
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->owned = true;
    d->generic = true;
    strncpy(d->name, name, HAPTIC_NAME_MAX);
    d->pin = pin;
    d->effects = HAPTIC_EFFECTS_ALL;
    d->ops = pinOps;
    d->ctx = pin;
    ctx->devs[ctx->count++] = d;
    return 0;
}

static int addEntry(struct vfs_driver_ctx_t *ctx, char *entry) {
    if (ctx->count >= CONFIG_WANTED_HAPTIC_MAX_DEVICES)
        return -E2BIG;
    char *rest = splitAt(entry, ':');
    if (rest == NULL) {
        if (!validToken(entry, strlen(entry)))
            return -EINVAL;
        return claimBoardDevice(ctx, entry);
    }
    return openPinMotor(ctx, entry, rest);
}

/* Parse motors=<entry>[,<entry>...][,observe]. The observe token applies to
 * the whole grant and is found before any entry opens. Returns 0 or a negative
 * errno; the caller destroys the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(HAPTIC_MOTORS_KEY) - 1;

    if (options == NULL || strncmp(options, HAPTIC_MOTORS_KEY, keyLen) != 0 ||
        options[keyLen] == '\0') {
        DEBUG_TRACE("haptic grant has no motors= clause");
        return -EINVAL;
    }

    size_t len = strlen(options + keyLen);
    char *work = WantedMalloc(len + 1);
    if (work == NULL)
        return -ENOMEM;
    memcpy(work, options + keyLen, len + 1);

    char *items[CONFIG_WANTED_HAPTIC_MAX_DEVICES + 1];
    size_t nItems = 0;
    int rc = 0;
    char *cur = work;
    while (cur != NULL && rc == 0) {
        char *item = cur;
        cur = splitAt(cur, ',');
        if (item[0] == '\0') {
            rc = -EINVAL;
        } else if (strcmp(item, HAPTIC_OBSERVE_TOKEN) == 0) {
            if (ctx->observe)
                rc = -EINVAL;
            ctx->observe = true;
        } else if (nItems >= CONFIG_WANTED_HAPTIC_MAX_DEVICES) {
            rc = -E2BIG;
        } else {
            items[nItems++] = item;
        }
    }
    if (nItems == 0)
        rc = -EINVAL;
    for (size_t i = 0; i < nItems && rc == 0; i++)
        rc = addEntry(ctx, items[i]);

    WantedFree(work);
    return rc;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *dev) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = HAPTIC_NODE_ROOT;
        *dev = 0;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;
    size_t nameLen = (size_t)(slash - p);

    int idx = -1;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strlen(ctx->devs[i]->name) == nameLen &&
            strncmp(ctx->devs[i]->name, p, nameLen) == 0)
            idx = i;
    }
    if (idx < 0)
        return -ENOENT;
    *dev = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0') {
        *node = HAPTIC_NODE_DEVICE;
        return 0;
    }
    if (strcmp(slash, "effects") == 0) {
        *node = HAPTIC_NODE_EFFECTS;
    } else if (strcmp(slash, "busy") == 0) {
        *node = HAPTIC_NODE_BUSY;
    } else if (strcmp(slash, "ctl") == 0 && !ctx->observe) {
        *node = HAPTIC_NODE_CTL;
    } else {
        return -ENOENT;
    }
    return 0;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsHapticInit(const wapp_t *wapp, const char *options) {
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
    driver->ReadDir = _ReadDir;

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = parseGrant(ctx, options);
    PlatformMutexUnlock(table.lock);
    if (rc < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

/* A writer's motor stops with its grant. A pin motor goes with the grant; a
 * board device stays registered and unowned. */
static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        PlatformMutexLock(table.lock);
        for (uint8_t i = 0; i < ctx->count && !ctx->observe; i++) {
            haptic_dev_t *m = ctx->devs[i];
            m->ops.stop(m->ctx);
            if (m->generic) {
                PlatformMotorClose(m->pin);
                memset(m, 0, sizeof(*m));
            } else {
                m->owned = false;
            }
        }
        PlatformMutexUnlock(table.lock);
        WantedFree(ctx);
    }
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct haptic_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= HAPTIC_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    uint8_t node;
    uint8_t dev;
    int rc = resolve(d, path != NULL ? path : "", &node, &dev);
    if (rc < 0)
        return rc;

    for (int i = 0; i < HAPTIC_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            d->fds[i].dev = dev;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct haptic_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct haptic_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == HAPTIC_NODE_ROOT || f->node == HAPTIC_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* The supported effect names, one per line. */
static size_t formatEffects(const haptic_dev_t *m, char *out, size_t outLen) {
    size_t n = 0;
    for (unsigned e = 0; e < HAPTIC_EFFECT_COUNT; e++) {
        if ((m->effects & (1U << e)) == 0)
            continue;
        n += (size_t)snprintf(out + n, outLen - n, "%s\n", effectNames[e]);
    }
    return n;
}

/* One text block, then EOF on this descriptor. Each open reads afresh. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct haptic_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == HAPTIC_NODE_ROOT || f->node == HAPTIC_NODE_DEVICE)
        return -EISDIR;
    if (f->node == HAPTIC_NODE_CTL)
        return -EPERM;
    if (f->done)
        return 0;

    haptic_dev_t *m = d->devs[f->dev];
    char text[HAPTIC_LINE_MAX * 2];
    size_t len;
    PlatformMutexLock(table.lock);
    if (f->node == HAPTIC_NODE_EFFECTS)
        len = formatEffects(m, text, sizeof(text));
    else
        len = (size_t)snprintf(text, sizeof(text), "%d\n",
                               m->ops.busy(m->ctx) ? 1 : 0);
    PlatformMutexUnlock(table.lock);

    size_t n = nbyte < len ? nbyte : len;
    memcpy(buf, text, n);
    f->done = true;
    return (int)n;
}

/* Decimal digits only, so a sign or a space is an error. */
static int parseNumber(const char *s, unsigned long *out) {
    size_t n = strlen(s);
    if (n == 0 || n > HAPTIC_DIGITS_MAX)
        return -EINVAL;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -EINVAL;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int playEffect(haptic_dev_t *m, const char *name) {
    for (unsigned e = 0; e < HAPTIC_EFFECT_COUNT; e++) {
        if (strcmp(name, effectNames[e]) == 0 && (m->effects & (1U << e)) != 0)
            return m->ops.play(m->ctx, e);
    }
    return -EINVAL;
}

/* "play <effect>", "pulse <ms>" or "stop". */
static int writeCtl(haptic_dev_t *m, char *line) {
    const char *arg = splitAt(line, ' ');
    unsigned long ms;
    if (strcmp(line, "stop") == 0) {
        if (arg != NULL)
            return -EINVAL;
        m->ops.stop(m->ctx);
        return 0;
    }
    if (arg == NULL)
        return -EINVAL;
    if (strcmp(line, "play") == 0)
        return playEffect(m, arg);
    if (strcmp(line, "pulse") == 0) {
        if (parseNumber(arg, &ms) < 0 || ms == 0 || ms > HAPTIC_PULSE_MS_MAX)
            return -EINVAL;
        return m->ops.pulse(m->ctx, (unsigned)ms);
    }
    return -EINVAL;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct haptic_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == HAPTIC_NODE_ROOT || f->node == HAPTIC_NODE_DEVICE)
        return -EISDIR;
    if (f->node != HAPTIC_NODE_CTL)
        return -EPERM;

    /* One line, with or without a trailing newline. */
    if (nbyte == 0 || nbyte >= HAPTIC_LINE_MAX)
        return -EINVAL;
    char line[HAPTIC_LINE_MAX];
    memcpy(line, buf, nbyte);
    line[nbyte] = '\0';
    if (line[nbyte - 1] == '\n')
        line[nbyte - 1] = '\0';

    PlatformMutexLock(table.lock);
    int rc = writeCtl(d->devs[f->dev], line);
    PlatformMutexUnlock(table.lock);
    return rc < 0 ? rc : (int)nbyte;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct haptic_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == HAPTIC_NODE_DEVICE) {
        vfs_dir_entry_t attrs[3];
        size_t n = 0;
        if (!d->observe) {
            attrs[n].name = "ctl";
            attrs[n++].type = VFS_FILETYPE_CHARACTER_DEVICE;
        }
        attrs[n].name = "effects";
        attrs[n++].type = VFS_FILETYPE_CHARACTER_DEVICE;
        attrs[n].name = "busy";
        attrs[n++].type = VFS_FILETYPE_CHARACTER_DEVICE;
        return VfsFlatDirReadDir(attrs, n, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != HAPTIC_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_HAPTIC_MAX_DEVICES];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->devs[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
