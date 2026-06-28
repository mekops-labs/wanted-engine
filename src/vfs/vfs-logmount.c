/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-config.h>
#include <wanted_malloc.h>

/* ── VfsLogMount ─────────────────────────────────────────────────────────────
 *
 * A read-only directory view of per-wapp captured logs, bound via the launch
 * config mounts[] at an operator-chosen path. Under the mount:
 *
 *   <mount>/            ReadDir → one entry per wapp with a live log slot
 *   <mount>/<name>      Read    → wapp <name>'s ring-buffered stdout/stderr
 *
 * The write side is unchanged: a wapp whose console is the "log" driver
 * (console:{"name":"log"}) appends here through vfs-log.c. This mount is the
 * read surface over the same process-wide LogStore, grantable independently of
 * the /dev/wanted control plane — a log shipper gets logs without control.
 *
 * The `name=<wapp>` option scopes the mount to a single wapp: a least-privilege
 * grant that exposes one log rather than the whole fleet. With no option the
 * mount exposes every wapp's log (the supervisor's broad view).
 * ───────────────────────────────────────────────────────────────────────── */

#define ID {'L', 'm', 'n', 't'}
#define LOGMOUNT_MAX_OPEN 8

typedef struct {
    bool in_use;
    bool is_root;
    char name[WAPP_MAX_NAME_LEN];
    bool read_done; /* per-fd EOF latch */
} logmount_fd_t;

struct vfs_driver_ctx_t {
    char scope[WAPP_MAX_NAME_LEN]; /* "" = all wapps; else only this one */
    logmount_fd_t fds[LOGMOUNT_MAX_OPEN];
};

static const char id[4] = ID;

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

/* Parse the mount's options for a `name=<wapp>` scope; absent → empty (all
 * wapps). Options are comma-separated; only `name=` is recognised here. */
static void parseScope(const char *opt, char *scope, size_t cap) {
    scope[0] = '\0';
    if (opt == NULL)
        return;
    const char *p = opt;
    while (*p != '\0') {
        if (strncmp(p, "name=", 5) == 0) {
            p += 5;
            size_t n = 0;
            while (p[n] != '\0' && p[n] != ',' && n < cap - 1)
                n++;
            memcpy(scope, p, n);
            scope[n] = '\0';
            return;
        }
        while (*p != '\0' && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
}

vfs_driver_t *VfsLogMountInit(const wapp_t *wapp, const char *opt) {
    (void)wapp;

    vfs_driver_t *driver = WantedMalloc(sizeof(*driver));
    if (driver == NULL)
        return NULL;
    memset(driver, 0, sizeof(*driver));

    driver->ctx = WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (driver->ctx == NULL) {
        WantedFree(driver);
        return NULL;
    }
    memset(driver->ctx, 0, sizeof(*driver->ctx));
    parseScope(opt, driver->ctx->scope, WAPP_MAX_NAME_LEN);

    driver->bytesId = *(const uint32_t *)id;
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->ReadDir = _ReadDir;
    /* No Write / OpenAt: the mount is a flat read-only view. */
    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d) {
        WantedFree(d->ctx);
        WantedFree(d);
    }
    return 0;
}

static int alloc_fd(vfs_driver_ctx_t d) {
    for (int i = 0; i < LOGMOUNT_MAX_OPEN; i++) {
        if (!d->fds[i].in_use)
            return i;
    }
    return -EMFILE;
}

/* True when `name` is within this mount's scope and has a live log slot. */
static bool visible(vfs_driver_ctx_t d, const char *name) {
    if (d->scope[0] != '\0' &&
        strncmp(d->scope, name, WAPP_MAX_NAME_LEN) != 0)
        return false;
    return LogStoreHas(LogStore(), name);
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    if (VFS_O_IS_WRITE(flags))
        return -EROFS;

    const char *p = path;
    while (*p == '/')
        p++;

    bool is_root = (*p == '\0');
    char name[WAPP_MAX_NAME_LEN];
    name[0] = '\0';

    if (!is_root) {
        size_t n = 0;
        while (p[n] != '\0' && p[n] != '/')
            n++;
        /* A wapp log is a leaf: reject a trailing path segment. */
        if (p[n] == '/' && p[n + 1] != '\0')
            return -ENOENT;
        if (n >= WAPP_MAX_NAME_LEN)
            return -ENAMETOOLONG;
        memcpy(name, p, n);
        name[n] = '\0';
        if (!visible(d, name))
            return -ENOENT;
    }

    int fd = alloc_fd(d);
    if (fd < 0)
        return fd;
    d->fds[fd].in_use = true;
    d->fds[fd].is_root = is_root;
    d->fds[fd].read_done = false;
    strncpy(d->fds[fd].name, name, WAPP_MAX_NAME_LEN - 1);
    d->fds[fd].name[WAPP_MAX_NAME_LEN - 1] = '\0';
    return fd;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= LOGMOUNT_MAX_OPEN)
        return -EBADF;
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    if (fd < 0 || fd >= LOGMOUNT_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = d->fds[fd].is_root ? VFS_FILETYPE_DIRECTORY
                                        : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (fd < 0 || fd >= LOGMOUNT_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;
    if (d->fds[fd].is_root)
        return -EISDIR;
    if (d->fds[fd].read_done)
        return 0; /* EOF, latched per-fd */

    size_t n = LogStoreRead(LogStore(), d->fds[fd].name, (char *)buf, nbyte);
    d->fds[fd].read_done = true;
    return (int)n;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (buf == NULL || cookie == NULL || bufUsed == NULL)
        return -EINVAL;
    if (fd < 0 || fd >= LOGMOUNT_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;
    if (!d->fds[fd].is_root)
        return -ENOTDIR;

    char names[LOG_SLOTS][WAPP_MAX_NAME_LEN];
    size_t total = LogStoreList(LogStore(), names, LOG_SLOTS);

    vfs_dir_entry_t entries[LOG_SLOTS];
    size_t count = 0;
    for (size_t i = 0; i < total; i++) {
        if (d->scope[0] != '\0' &&
            strncmp(d->scope, names[i], WAPP_MAX_NAME_LEN) != 0)
            continue;
        entries[count].name = names[i];
        entries[count].type = VFS_FILETYPE_CHARACTER_DEVICE;
        count++;
    }
    return VfsFlatDirReadDir(entries, count, buf, bufLen, cookie, bufUsed);
}
