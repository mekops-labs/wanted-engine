#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-devfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* DevFs — direct table-backed lookup.
 *
 * The handle owns no driver state; it just remembers the (drv, drv_fd) pair
 * so subsequent ops can dispatch without rewalking the table. Driver lifetime
 * is owned by DevFs itself: DevFs_Destroy walks the table on VfsDestroy. */

typedef struct devfs_handle_t {
    const vfs_driver_t *drv;
    int drv_fd;
} devfs_handle_t;

static const vfs_driver_t *LookupDrv(vfs_ctx_t c, const char *suffix) {
    if (!c || !suffix || *suffix == '\0')
        return NULL;
    for (uint8_t i = 0; i < c->devfs_cnt; i++) {
        if (strncmp(c->devfs[i].name, suffix, MAX_ENTRY_NAME_LEN) == 0)
            return c->devfs[i].drv;
    }
    return NULL;
}

int DevFs_Register(vfs_ctx_t c, const char *name, const vfs_driver_t *driver) {
    if (!c || !name || *name == '\0' || !driver)
        return -EINVAL;
    if (strlen(name) >= MAX_ENTRY_NAME_LEN)
        return -ENAMETOOLONG;
    if (c->devfs_cnt >= VFS_DEVFS_MAX_ENTRIES)
        return -ENOSPC;

    /* Reject duplicate name to keep lookup deterministic. */
    for (uint8_t i = 0; i < c->devfs_cnt; i++) {
        if (strncmp(c->devfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return -EEXIST;
    }

    vfs_named_drv_t *e = &c->devfs[c->devfs_cnt++];
    strncpy(e->name, name, MAX_ENTRY_NAME_LEN - 1);
    e->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
    e->drv = driver;
    DEBUG_TRACE("/dev/%s -> %.4s", e->name, driver->id);
    return 0;
}

void DevFs_Destroy(vfs_ctx_t c) {
    if (!c)
        return;
    for (uint8_t i = 0; i < c->devfs_cnt; i++) {
        const vfs_driver_t *d = c->devfs[i].drv;
        if (d && d->Destroy)
            d->Destroy((vfs_driver_t *)d);
        c->devfs[i].drv = NULL;
        c->devfs[i].name[0] = '\0';
    }
    c->devfs_cnt = 0;
}

void *DevFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags) {
    DEBUG_TRACE("/dev/%s (0x%x)", suffix ? suffix : "(null)", flags);

    const vfs_driver_t *drv = LookupDrv(c, suffix);
    if (!drv)
        return NULL;

    int drv_fd = TRY_DRV(drv, Open, "", flags);
    if (drv_fd < 0)
        return NULL;

    devfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        TRY_DRV(drv, Close, drv_fd);
        return NULL;
    }
    h->drv = drv;
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
