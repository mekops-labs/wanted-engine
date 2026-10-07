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
#include <rtc-chip.h>
#include <rtc-time.h>
#include <vfs-drivers.h>
#include <vfs-rtc.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

/* RTC driver: /dev/rtc/<name>/{time,status,source}, one granted device per
 * wapp. Devices live in an engine-wide table, so every wapp that names one
 * sees the same clock. The device named `main` drives the system clock. */

static const char id[] = {'R', 't', 'C', 'd'};

#define RTC_MAX_FDS 8
#define RTC_DEVICES_KEY "devices="
#define RTC_SET_CLAUSE "set"
#define RTC_MAIN_NAME "main"

#define RTC_NODE_ROOT 0
#define RTC_NODE_DEVICE 1
#define RTC_NODE_TIME 2
#define RTC_NODE_STATUS 3
#define RTC_NODE_SOURCE 4

#define RTC_LINE_MAX 32
#define RTC_NS_PER_S 1000000000ULL

typedef struct rtc_dev_t {
    bool used;
    bool soft; /* the time is the system clock */
    bool softValid;
    char name[RTC_NAME_MAX + 1];
    rtc_ops_t ops;
    void *ctx;
    rtc_source_t source;
} rtc_dev_t;

static struct {
    rtc_dev_t devs[CONFIG_WANTED_RTC_MAX_DEVICES];
    platform_mutex_t *lock;
} table;

struct rtc_fd_t {
    bool used;
    bool done;
    uint8_t node;
};

struct vfs_driver_ctx_t {
    rtc_dev_t *dev;
    bool canSet;
    struct rtc_fd_t fds[RTC_MAX_FDS];
};

/* ── Software device ─────────────────────────────────────────────────────── */

static int softGet(void *ctx, uint32_t *sec, bool *valid) {
    const rtc_dev_t *d = ctx;
    plat_timestamp_t ns = 0;
    int rc = PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &ns);
    if (rc < 0)
        return rc;
    *sec = (uint32_t)(ns / RTC_NS_PER_S);
    *valid = d->softValid;
    return 0;
}

/* The write path sets the system clock itself; the device holds nothing. */
static int softSet(void *ctx, uint32_t sec) {
    (void)ctx;
    (void)sec;
    return 0;
}

/* ── Device table ────────────────────────────────────────────────────────── */

/* [A-Za-z0-9_-], 1..RTC_NAME_MAX characters. */
static bool validToken(const char *s, size_t len) {
    if (len == 0 || len > RTC_NAME_MAX)
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

static void ensureLock(void) {
    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
}

static rtc_dev_t *findDevice(const char *name, size_t len) {
    for (size_t i = 0; i < CONFIG_WANTED_RTC_MAX_DEVICES; i++) {
        rtc_dev_t *d = &table.devs[i];
        if (d->used && strlen(d->name) == len &&
            strncmp(d->name, name, len) == 0)
            return d;
    }
    return NULL;
}

/* Caller holds the lock. */
static rtc_dev_t *addDevice(const char *name, const rtc_ops_t *ops, void *ctx,
                            int *rc) {
    if (findDevice(name, strlen(name)) != NULL) {
        *rc = -EEXIST;
        return NULL;
    }
    for (size_t i = 0; i < CONFIG_WANTED_RTC_MAX_DEVICES; i++) {
        rtc_dev_t *d = &table.devs[i];
        if (d->used)
            continue;
        memset(d, 0, sizeof(*d));
        d->used = true;
        strncpy(d->name, name, RTC_NAME_MAX);
        d->ops = *ops;
        d->ctx = ctx;
        d->source = RTC_SRC_NONE;
        *rc = 0;
        return d;
    }
    *rc = -ENOSPC;
    return NULL;
}

int RtcDeviceRegister(const rtc_device_desc_t *desc) {
    if (desc == NULL || desc->name == NULL ||
        !validToken(desc->name, strlen(desc->name)) || desc->ops == NULL ||
        desc->ops->get == NULL || desc->ops->set == NULL)
        return -EINVAL;

    ensureLock();
    PlatformMutexLock(table.lock);
    int rc = 0;
    addDevice(desc->name, desc->ops, desc->ctx, &rc);
    PlatformMutexUnlock(table.lock);
    return rc;
}

/* Every engine offers `main`: the software device when no board did. Caller
 * holds the lock. */
static rtc_dev_t *ensureMain(void) {
    rtc_dev_t *d = findDevice(RTC_MAIN_NAME, sizeof(RTC_MAIN_NAME) - 1);
    if (d != NULL)
        return d;

    static const rtc_ops_t ops = {softGet, softSet};
    int rc = 0;
    d = addDevice(RTC_MAIN_NAME, &ops, NULL, &rc);
    if (d != NULL) {
        d->soft = true;
        d->ctx = d;
    }
    return d;
}

static bool isMain(const rtc_dev_t *d) {
    return strcmp(d->name, RTC_MAIN_NAME) == 0;
}

void RtcDevicesReset(void) {
    RtcChipsReset();
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

/* ── System clock ────────────────────────────────────────────────────────── */

static int setSystemClock(uint32_t sec) {
    return PlatformClockSetTime(PLAT_CLOCKID_REALTIME,
                                (plat_timestamp_t)sec * RTC_NS_PER_S);
}

bool RtcBoot(void) {
    ensureLock();
    PlatformMutexLock(table.lock);
    rtc_dev_t *d = ensureMain();
    bool applied = false;
    uint32_t sec = 0;
    bool valid = false;
    if (d != NULL && !d->soft && d->ops.get(d->ctx, &sec, &valid) == 0 &&
        valid && setSystemClock(sec) == 0) {
        d->source = RTC_SRC_RTC;
        WantedSetClockQuality(RtcSourceQuality(RTC_SRC_RTC));
        applied = true;
    }
    PlatformMutexUnlock(table.lock);
    return applied;
}

/* The order of a write: the device, then the system clock, then the source
 * and the quality byte. Caller holds the lock. */
static int applyWrite(rtc_dev_t *d, uint32_t sec, rtc_source_t src) {
    if (d->soft) {
        if (setSystemClock(sec) < 0)
            return -EIO;
        d->softValid = true;
    } else {
        if (d->ops.set(d->ctx, sec) < 0)
            return -EIO;
        if (isMain(d) && setSystemClock(sec) < 0)
            return -EIO;
    }
    d->source = src;
    if (isMain(d))
        WantedSetClockQuality(RtcSourceQuality(src));
    return 0;
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse devices=<name>[,set]. Returns 0 or a negative errno; the caller
 * destroys the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(RTC_DEVICES_KEY) - 1;
    if (options == NULL || strncmp(options, RTC_DEVICES_KEY, keyLen) != 0) {
        DEBUG_TRACE("rtc grant has no devices= clause");
        return -EINVAL;
    }

    const char *name = options + keyLen;
    const char *comma = strchr(name, ',');
    size_t nameLen = comma != NULL ? (size_t)(comma - name) : strlen(name);
    if (!validToken(name, nameLen))
        return -EINVAL;
    if (comma != NULL) {
        if (strcmp(comma + 1, RTC_SET_CLAUSE) != 0) {
            DEBUG_TRACE("rtc grant has an unknown clause");
            return -EINVAL;
        }
        ctx->canSet = true;
    }

    if (nameLen == sizeof(RTC_MAIN_NAME) - 1 &&
        strncmp(name, RTC_MAIN_NAME, nameLen) == 0)
        ensureMain();
    ctx->dev = findDevice(name, nameLen);
    if (ctx->dev == NULL) {
        DEBUG_TRACE("rtc grant names an unknown device");
        return -EINVAL;
    }
    return 0;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static const struct {
    const char *name;
    uint8_t node;
} nodes[] = {
    {"time", RTC_NODE_TIME},
    {"status", RTC_NODE_STATUS},
    {"source", RTC_NODE_SOURCE},
};
#define RTC_NODE_COUNT (sizeof(nodes) / sizeof(nodes[0]))

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = RTC_NODE_ROOT;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;
    size_t nameLen = (size_t)(slash - p);
    if (strlen(ctx->dev->name) != nameLen ||
        strncmp(ctx->dev->name, p, nameLen) != 0)
        return -ENOENT;

    while (*slash == '/')
        slash++;
    if (*slash == '\0') {
        *node = RTC_NODE_DEVICE;
        return 0;
    }
    for (size_t i = 0; i < RTC_NODE_COUNT; i++) {
        if (strcmp(slash, nodes[i].name) == 0) {
            *node = nodes[i].node;
            return 0;
        }
    }
    return -ENOENT;
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

vfs_driver_t *VfsRtcInit(const wapp_t *wapp, const char *options) {
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

    ensureLock();
    PlatformMutexLock(table.lock);
    int rc = parseGrant(ctx, options);
    PlatformMutexUnlock(table.lock);
    if (rc < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct rtc_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= RTC_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    int rc = resolve(d, path != NULL ? path : "", &node);
    if (rc < 0)
        return rc;

    if (VFS_O_IS_WRITE(flags) &&
        (node == RTC_NODE_STATUS || node == RTC_NODE_SOURCE ||
         (node == RTC_NODE_TIME && !d->canSet)))
        return -EACCES;

    for (int i = 0; i < RTC_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct rtc_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct rtc_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == RTC_NODE_ROOT || f->node == RTC_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* Fill `line` for a node. Caller holds the lock. */
static int formatNode(const rtc_dev_t *dev, uint8_t node, char *line,
                      size_t lineLen) {
    if (node == RTC_NODE_SOURCE) {
        snprintf(line, lineLen, "%s\n", RtcSourceName(dev->source));
        return 0;
    }

    uint32_t sec = 0;
    bool valid = false;
    if (dev->ops.get(dev->ctx, &sec, &valid) < 0)
        return -EIO;
    if (node == RTC_NODE_STATUS) {
        snprintf(line, lineLen, "%s\n", valid ? "valid" : "invalid");
        return 0;
    }
    if (!valid)
        return -EIO;
    snprintf(line, lineLen, "%lu\n", (unsigned long)sec);
    return 0;
}

/* One text line, then EOF on this descriptor. Each open reads afresh. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct rtc_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == RTC_NODE_ROOT || f->node == RTC_NODE_DEVICE)
        return -EISDIR;
    if (f->done)
        return 0;

    char line[RTC_LINE_MAX];
    PlatformMutexLock(table.lock);
    int rc = formatNode(d->dev, f->node, line, sizeof(line));
    PlatformMutexUnlock(table.lock);
    if (rc < 0)
        return rc;

    size_t len = strlen(line);
    size_t n = nbyte < len ? nbyte : len;
    memcpy(buf, line, n);
    f->done = true;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct rtc_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == RTC_NODE_ROOT || f->node == RTC_NODE_DEVICE)
        return -EISDIR;
    if (f->node != RTC_NODE_TIME || !d->canSet)
        return -EACCES;

    uint32_t sec;
    rtc_source_t src;
    int rc = RtcParseWrite(buf, nbyte, &sec, &src);
    if (rc < 0)
        return rc;

    PlatformMutexLock(table.lock);
    rc = applyWrite(d->dev, sec, src);
    PlatformMutexUnlock(table.lock);
    return rc < 0 ? rc : (int)nbyte;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct rtc_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == RTC_NODE_DEVICE) {
        vfs_dir_entry_t attrs[RTC_NODE_COUNT];
        for (size_t i = 0; i < RTC_NODE_COUNT; i++) {
            attrs[i].name = nodes[i].name;
            attrs[i].type = VFS_FILETYPE_CHARACTER_DEVICE;
        }
        return VfsFlatDirReadDir(attrs, RTC_NODE_COUNT, buf, bufLen, cookie,
                                 bufUsed);
    }
    if (f->node != RTC_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entry = {d->dev->name, VFS_FILETYPE_DIRECTORY};
    return VfsFlatDirReadDir(&entry, 1, buf, bufLen, cookie, bufUsed);
}
