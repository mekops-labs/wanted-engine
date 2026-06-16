/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-netfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* NetFs — direct table-backed lookup.
 *
 * Mirrors DevFs: WantedInstallDriver registers each "/net/<name>" driver into
 * the per-wapp table; NetFs_Open exact-matches the suffix and forwards into
 * the matched driver's Open. Driver lifetime is owned by NetFs. */

typedef struct netfs_handle_t {
    const vfs_driver_t *drv;
    int drv_fd;
    bool is_root; /* true when opening "/net" itself as a directory */
} netfs_handle_t;

static const vfs_driver_t *lookupDrv(vfs_ctx_t c, const char *suffix) {
    if (!c || !suffix || *suffix == '\0')
        return NULL;
    for (uint8_t i = 0; i < c->netfs_cnt; i++) {
        if (strncmp(c->netfs[i].name, suffix, MAX_ENTRY_NAME_LEN) == 0)
            return c->netfs[i].drv;
    }
    return NULL;
}

int NetFs_Register(vfs_ctx_t c, const char *name, const vfs_driver_t *driver) {
    if (!c || !name || *name == '\0' || !driver)
        return -EINVAL;
    if (strlen(name) >= MAX_ENTRY_NAME_LEN)
        return -ENAMETOOLONG;
    if (c->netfs_cnt >= VFS_DEVFS_MAX_ENTRIES)
        return -ENOSPC;

    for (uint8_t i = 0; i < c->netfs_cnt; i++) {
        if (strncmp(c->netfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return -EEXIST;
    }

    vfs_named_drv_t *e = &c->netfs[c->netfs_cnt++];
    strncpy(e->name, name, MAX_ENTRY_NAME_LEN - 1);
    e->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
    e->drv = driver;
    DEBUG_TRACE("/net/%s -> %.4s", e->name, driver->id);
    return 0;
}

void NetFs_Destroy(vfs_ctx_t c) {
    if (!c)
        return;
    for (uint8_t i = 0; i < c->netfs_cnt; i++) {
        const vfs_driver_t *d = c->netfs[i].drv;
        if (d && d->Destroy)
            d->Destroy((vfs_driver_t *)d);
        c->netfs[i].drv = NULL;
        c->netfs[i].name[0] = '\0';
    }
    c->netfs_cnt = 0;
}

void *NetFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                 int *out_err) {
    DEBUG_TRACE("/net/%s (0x%x)", suffix ? suffix : "(null)", flags);

    /* Empty suffix means opening "/net" itself as a directory. */
    if (!suffix || *suffix == '\0') {
        netfs_handle_t *h = WantedMalloc(sizeof(*h));
        if (!h) {
            if (out_err)
                *out_err = -ENOMEM;
            return NULL;
        }
        h->drv = NULL;
        h->drv_fd = -1;
        h->is_root = true;
        if (out_err)
            *out_err = 0;
        return h;
    }

    const vfs_driver_t *drv = lookupDrv(c, suffix);
    if (!drv) {
        if (out_err)
            *out_err = -ENOENT;
        return NULL;
    }

    int drv_fd = TRY_DRV(drv, Open, "", flags);
    if (drv_fd < 0) {
        if (out_err)
            *out_err = drv_fd;
        return NULL;
    }

    netfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        TRY_DRV(drv, Close, drv_fd);
        if (out_err)
            *out_err = -ENOMEM;
        return NULL;
    }
    if (out_err)
        *out_err = 0;
    h->drv = drv;
    h->drv_fd = drv_fd;
    h->is_root = false;
    return h;
}

int NetFs_Close(vfs_ctx_t c, void *handle) {
    (void)c;
    netfs_handle_t *h = handle;
    if (!h)
        return -EBADF;
    int r = 0;
    if (!h->is_root)
        r = TRY_DRV(h->drv, Close, h->drv_fd);
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
    if (!h)
        return -EBADF;
    if (h->is_root) {
        memset(stat, 0, sizeof(*stat));
        stat->filetype = VFS_FILETYPE_DIRECTORY;
        return 0;
    }
    return TRY_DRV(h->drv, Stat, h->drv_fd, stat);
}

int NetFs_StatPath(vfs_ctx_t c, const char *suffix, vfs_stat_t *stat) {
    if (!stat)
        return -EINVAL;
    memset(stat, 0, sizeof(*stat));

    /* Empty suffix is "/net" itself. */
    if (!suffix || *suffix == '\0') {
        stat->filetype = VFS_FILETYPE_DIRECTORY;
        return 0;
    }

    const vfs_driver_t *drv = lookupDrv(c, suffix);
    if (!drv)
        return -ENOENT;
    stat->filetype = drv->filetype;
    return 0;
}

int NetFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed) {
    netfs_handle_t *h = handle;
    if (!h)
        return -EBADF;

    if (h->is_root) {
        vfs_dir_entry_t entries[VFS_DEVFS_MAX_ENTRIES];
        for (uint8_t i = 0; i < c->netfs_cnt; i++) {
            entries[i].name = c->netfs[i].name;
            entries[i].type = TRY_FILETYPE(c->netfs[i].drv);
        }
        return VfsFlatDirReadDir(entries, c->netfs_cnt, buf, bufLen, cookie,
                                 bufUsed);
    }

    return TRY_DRV(h->drv, ReadDir, h->drv_fd, buf, bufLen, cookie, bufUsed);
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
