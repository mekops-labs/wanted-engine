/* SPDX-License-Identifier: Apache-2.0 */

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
    bool is_root; /* true when opening "/dev" itself as a directory */
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

void *DevFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                 int *out_err) {
    DEBUG_TRACE("/dev/%s (0x%x)", suffix ? suffix : "(null)", flags);

    /* Empty suffix means opening "/dev" itself as a directory. */
    if (!suffix || *suffix == '\0') {
        if (!c) {
            if (out_err)
                *out_err = -EINVAL;
            return NULL;
        }
        devfs_handle_t *h = WantedMalloc(sizeof(*h));
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

    if (!c) {
        if (out_err)
            *out_err = -EINVAL;
        return NULL;
    }

    const vfs_driver_t *drv = LookupDrv(c, suffix);
    const char *sub_path = NULL;

    if (!drv) {
        /* Prefix match: entry "X" matches suffix "X/rest", passes "rest" down. */
        for (uint8_t i = 0; i < c->devfs_cnt; i++) {
            size_t nlen = strnlen(c->devfs[i].name, MAX_ENTRY_NAME_LEN);
            if (nlen > 0 && strncmp(c->devfs[i].name, suffix, nlen) == 0 &&
                suffix[nlen] == '/') {
                drv = c->devfs[i].drv;
                sub_path = suffix + nlen + 1;
                break;
            }
        }
    }

    if (!drv) {
        if (out_err)
            *out_err = -ENOENT;
        return NULL;
    }

    const char *open_path =
        sub_path ? sub_path
                 : (drv->filetype == VFS_FILETYPE_DIRECTORY ? "/" : "");
    int drv_fd = TRY_DRV(drv, Open, open_path, flags);
    if (drv_fd < 0) {
        if (out_err)
            *out_err = drv_fd;
        return NULL;
    }

    devfs_handle_t *h = WantedMalloc(sizeof(*h));
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

int DevFs_Close(vfs_ctx_t c, void *handle) {
    (void)c;
    devfs_handle_t *h = handle;
    if (!h)
        return -EBADF;
    int r = 0;
    if (!h->is_root)
        r = TRY_DRV(h->drv, Close, h->drv_fd);
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
    if (!h)
        return -EBADF;
    if (h->is_root) {
        memset(stat, 0, sizeof(*stat));
        stat->filetype = VFS_FILETYPE_DIRECTORY;
        return 0;
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
    devfs_handle_t *h = handle;
    if (!h)
        return -EBADF;

    if (h->is_root) {
        /* Enumerate all registered DevFs entries (e.g. "wanted"). */
        size_t used = 0;
        uint8_t i = (uint8_t)*cookie;
        for (; i < c->devfs_cnt; i++) {
            size_t namlen = strnlen(c->devfs[i].name, MAX_ENTRY_NAME_LEN);
            if (used + sizeof(vfs_dirent_t) + namlen > bufLen)
                break;
            vfs_dirent_t dir = {0};
            dir.d_ino = i;
            dir.d_namlen = (uint32_t)namlen;
            dir.d_type = TRY_FILETYPE(c->devfs[i].drv);
            dir.d_next = i + 1;
            memcpy((uint8_t *)buf + used, &dir, sizeof(dir));
            memcpy((uint8_t *)buf + used + sizeof(dir), c->devfs[i].name,
                   namlen);
            used += sizeof(dir) + namlen;
        }
        *cookie = i;
        *bufUsed = used;
        return 0;
    }

    (void)c;
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
