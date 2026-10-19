/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-vfs-api.h>

/* WantedRegCapacityDriver: the registry's stores as one read-only JSON array,
 * read whole from a single open. It is a node of its own, so `reg` holds image
 * entries only. */

#define CAPACITY_ID {'W', 'c', 'a', 'p'}
#define CAPACITY_MAX 640

static struct {
    bool opened;
    char body[CAPACITY_MAX];
    size_t len;
    size_t off;
} ctx;

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

const vfs_driver_t WantedRegCapacityDriver = {
    .id = CAPACITY_ID,
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Destroy = _Destroy,
    .Open = _Open,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
    .Write = _Write,
};

static int _Destroy(struct vfs_driver_t *d) {
    (void)d;
    memset(&ctx, 0, sizeof(ctx));
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    int n;

    (void)d;
    (void)path;
    if (flags & (VFS_O_WRONLY | VFS_O_RDWR))
        return -EROFS;
    if (ctx.opened)
        return -EBUSY;
    n = WantedRenderRegistryCapacity(ctx.body, sizeof(ctx.body));
    if (n < 0)
        return n;
    ctx.len = (size_t)n;
    ctx.off = 0;
    ctx.opened = true;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d;
    (void)fd;
    ctx.opened = false;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)d;
    (void)fd;
    if (stat == NULL)
        return -EINVAL;
    stat->dev = WantedRegCapacityDriver.bytesId;
    stat->ino = 0;
    stat->filetype = WantedRegCapacityDriver.filetype;
    stat->nlink = 0;
    stat->size = ctx.len;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    size_t n;

    (void)d;
    (void)fd;
    if (buf == NULL)
        return -EINVAL;
    if (!ctx.opened)
        return -EBADF;
    n = ctx.len - ctx.off;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, ctx.body + ctx.off, n);
    ctx.off += n;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    (void)buf;
    (void)nbyte;
    return -EROFS;
}
