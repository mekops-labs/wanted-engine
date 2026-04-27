#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-devfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* DevFs Phase 4 shim — forwards into the per-wapp legacy rootDriver subtree.
 * The handle owns no driver state; it just remembers which (drv, drv_fd) pair
 * the typed-FD entry points at, so Read/Write/Close/etc. can call straight
 * through. Phase 6 swaps the forward-through body for a direct table lookup. */

#define DEVFS_PATH_MAX 128

typedef struct devfs_handle_t {
    const vfs_driver_t *drv;
    int drv_fd;
} devfs_handle_t;

static int forward_open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags) {
    char path[DEVFS_PATH_MAX];
    int n;

    if (!c || !c->rootDriver || !suffix || *suffix == '\0') {
        return -EINVAL;
    }

    n = snprintf(path, sizeof(path), "/dev/%s", suffix);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }

    return TRY_DRV(c->rootDriver, OpenAt, c->fildes[ROOT_FD].drv_fd, path,
                   flags);
}

void *DevFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags) {
    DEBUG_TRACE("/dev/%s (0x%x)", suffix ? suffix : "(null)", flags);

    int drv_fd = forward_open(c, suffix, flags);
    if (drv_fd < 0) {
        return NULL;
    }

    devfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        TRY_DRV(c->rootDriver, Close, drv_fd);
        return NULL;
    }
    h->drv = c->rootDriver;
    h->drv_fd = drv_fd;
    return h;
}

int DevFs_Close(vfs_ctx_t c, void *handle) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    int r = TRY_DRV(h->drv, Close, h->drv_fd);
    WantedFree(h);
    return r;
}

int DevFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Read, h->drv_fd, buf, nbyte);
}

int DevFs_Write(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Write, h->drv_fd, buf, nbyte);
}

int DevFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Stat, h->drv_fd, stat);
}

int DevFs_StatSet(vfs_ctx_t c, void *handle, vfs_stat_t stat) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, StatSet, h->drv_fd, stat);
}

int DevFs_Seek(vfs_ctx_t c, void *handle, long off, vfs_whence_t whence,
               long *pos) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Seek, h->drv_fd, off, whence, pos);
}

int DevFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, ReadDir, h->drv_fd, buf, bufLen, cookie, bufUsed);
}

int DevFs_Unlink(vfs_ctx_t c, void *handle, const char *path) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Unlink, h->drv_fd, path);
}
