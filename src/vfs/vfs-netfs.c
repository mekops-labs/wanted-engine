#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-netfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* NetFs Phase 4 shim — forwards into the per-wapp legacy socket driver via
 * the rootDriver subtree, mirroring DevFs. Phase 6 will collapse the
 * forward-through into a direct call once the socket driver is owned
 * per-wapp outside of virt. */

#define NETFS_PATH_MAX 128

typedef struct netfs_handle_t {
    const vfs_driver_t *drv;
    int drv_fd;
} netfs_handle_t;

static int forward_open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags) {
    char path[NETFS_PATH_MAX];
    int n;

    if (!c || !c->rootDriver || !suffix || *suffix == '\0') {
        return -EINVAL;
    }

    n = snprintf(path, sizeof(path), "/net/%s", suffix);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }

    return TRY_DRV(c->rootDriver, OpenAt, c->fildes[ROOT_FD].drv_fd, path,
                   flags);
}

void *NetFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags) {
    DEBUG_TRACE("/net/%s (0x%x)", suffix ? suffix : "(null)", flags);

    int drv_fd = forward_open(c, suffix, flags);
    if (drv_fd < 0) {
        return NULL;
    }

    netfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        TRY_DRV(c->rootDriver, Close, drv_fd);
        return NULL;
    }
    h->drv = c->rootDriver;
    h->drv_fd = drv_fd;
    return h;
}

int NetFs_Close(vfs_ctx_t c, void *handle) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    int r = TRY_DRV(h->drv, Close, h->drv_fd);
    WantedFree(h);
    return r;
}

int NetFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Read, h->drv_fd, buf, nbyte);
}

int NetFs_Write(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Write, h->drv_fd, buf, nbyte);
}

int NetFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, Stat, h->drv_fd, stat);
}

int NetFs_SockAccept(vfs_ctx_t c, void *handle, vfs_oflags_t flags,
                     int *newFd) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, SockAccept, h->drv_fd, flags, newFd);
}

int NetFs_SockRecv(vfs_ctx_t c, void *handle, void *buf, size_t nbyte,
                   vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, SockRecv, h->drv_fd, buf, nbyte, iflags, oflags);
}

int NetFs_SockSend(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte,
                   vfs_sdflags_t flags) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, SockSend, h->drv_fd, buf, nbyte, flags);
}

int NetFs_SockShutdown(vfs_ctx_t c, void *handle, vfs_sdflags_t flags) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h) {
        return -EBADF;
    }
    return TRY_DRV(h->drv, SockShutdown, h->drv_fd, flags);
}
