/* SPDX-License-Identifier: Apache-2.0 */

#include <vfs-drivers.h>
#include <vfs.h>

#define ID                                                                     \
    { 'N', 'u', 'l', 'l' }

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

static vfs_driver_t NullDriver = {
    .id = ID,
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .ctx = NULL,
    .Destroy = _Destroy,
    .Open = _Open,
    .OpenAt = _OpenAt,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
    .Write = _Write,
};

vfs_driver_t *VfsNullInit(const wapp_t *wapp, const char *opt) {
    return &NullDriver;
}

static int _Destroy(struct vfs_driver_t *d) { return 0; }

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    return 0;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) { return 0; }

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    stat->dev = NullDriver.bytesId;
    stat->ino = 0;
    stat->filetype = NullDriver.filetype;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    return nbyte;
}
