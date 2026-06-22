/* SPDX-License-Identifier: Apache-2.0 */

/* WiFi station node for the Linux target. The radio is a NuttX hardware
 * capability; on Linux the engine has no station interface to drive, so the
 * node accepts an open and reports the operation as unsupported. It keeps the
 * `wifi` driver resolvable on every target. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'W', 'i', 'f', 'i'};

#define WIFI_MAX_FDS 2

struct vfs_driver_ctx_t {
    bool used[WIFI_MAX_FDS];
};

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)path;
    (void)flags;
    for (int i = 0; i < WIFI_MAX_FDS; i++) {
        if (!d->used[i]) {
            d->used[i] = true;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->used[fd])
        return -EBADF;
    d->used[fd] = false;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    (void)d;
    (void)fd;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    s->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    (void)buf;
    (void)nbyte;
    return -ENOTSUP;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    (void)buf;
    (void)nbyte;
    return -ENOTSUP;
}

vfs_driver_t *VfsWifiInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;
    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;

    return driver;
}
