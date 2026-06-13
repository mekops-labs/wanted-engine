/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-stdio.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Stdio DevFS aliases — /dev/stdin, /dev/stdout, /dev/stderr.
 *
 * These are not independent backings: each forwards to the wapp's own console
 * stream (WASI fd 0/1/2), so opening "/dev/stdout" and writing reaches exactly
 * the same place fd 1 does — the platform console, the log ring, or /dev/null,
 * whichever the launch config wired the slot to. The alias owns no console
 * state; it holds the target driver + that slot's driver-fd and delegates. The
 * target driver is owned by its STREAM slot (VfsDestroy tears it down), so the
 * alias never destroys it. */

struct vfs_driver_ctx_t {
    const vfs_driver_t *target; /* the console slot's driver (may be NULL) */
    int target_fd;              /* the slot's driver-fd (0/1/2) */
};

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)d; (void)path; (void)flags;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d; (void)fd;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)d; (void)fd;
    if (NULL == stat)
        return -EINVAL;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)fd;
    if (NULL == d || NULL == d->target)
        return 0; /* no console backing wired → EOF */
    return TRY_DRV(d->target, Read, d->target_fd, buf, nbyte);
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)fd;
    if (NULL == d || NULL == d->target)
        return (int)nbyte; /* no console backing wired → discard */
    return TRY_DRV(d->target, Write, d->target_fd, buf, nbyte);
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d) {
        WantedFree(d->ctx);
        WantedFree(d);
    }
    return 0;
}

vfs_driver_t *VfsStdioAliasInit(const vfs_driver_t *target, int target_fd) {
    static const char id[] = {'s', 't', 'd', 'a'};

    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver)
        return NULL;
    memset(driver, 0, sizeof(*driver));

    driver->ctx =
        (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        WantedFree(driver);
        return NULL;
    }

    driver->ctx->target = target;
    driver->ctx->target_fd = target_fd;
    driver->bytesId = *(const uint32_t *)id;
    driver->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Destroy = _Destroy;
    return driver;
}
