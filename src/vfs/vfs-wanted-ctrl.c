#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <vfs.h>
#include <vfs-drivers.h>

#include <platform.h>

#include <wanted-vfs-api.h>


#define ID  {'W', 'c', 't', 'l'}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos);

const vfs_driver_t WantedControlDriver = {
    .id              = ID,
    .filetype        = VFS_FILETYPE_CHARACTER_DEVICE,
    .Open            = _Open,
    .Close           = _Close,
    .Stat            = _Stat,
    .Read            = _Read,
    .Write           = _Write,
};

static bool opened = false;

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    if (opened) return -EBUSY;
    opened = true;

    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    opened = false;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    stat->dev = WantedControlDriver.bytesId;
    stat->ino = 0;
    stat->filetype = WantedControlDriver.filetype;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    if (buf == NULL) return -EINVAL;

    if (!opened) return -EBADF;

    static int read = 0;
    if (read > 0) {
        read = 0;
        return read;
    }

    read = WantedReadState(buf, nbyte);

    return read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    wapp_t wapp;
    int ret;

    ret = LoadWapp(buf, &wapp);
    if (ret < 0) {
        return ret;
    }

    ret = StartWapp(wapp);
    if (ret == 0) {
        return nbyte;
    }

    return ret;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos)
{
    return 0;
}
