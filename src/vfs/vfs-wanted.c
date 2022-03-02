#include <string.h>
#include <errno.h>

#include <vfs.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>
#include <debug_trace.h>

#include <cwalk.h>

#include "vfs-internal.h"

static const char id[] = { 'W', 'a', 'n', 't' };

struct vfs_driver_ctx_t {
    vfs_driver_t *fsDrv;
};

static vfs_entry_t files[] = {
    { "/",      NULL, VFS_FILETYPE_DIRECTORY },
    { "ctrl",   NULL, VFS_FILETYPE_CHARACTER_DEVICE },
    { "config", NULL, VFS_FILETYPE_CHARACTER_DEVICE },
    { "reg",    NULL, VFS_FILETYPE_DIRECTORY },         //< last entry is the registry directory provided by external driver
};

#define FILES_CNT (sizeof(files)/sizeof(files[0]))

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);

static inline
bool CheckFd(vfs_driver_ctx_t d, int fd)
{
    if (fd < 0) return false;

    return true;
}


int VfsWantedInit(vfs_driver_t *driver, vfs_driver_t *fileDriver)
{
    int ret;

    if (NULL == driver || NULL == fileDriver) {
        return -EINVAL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) return -ENOMEM;

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = VFS_FILETYPE_DIRECTORY;
    driver->ctx->fsDrv      = fileDriver;
    driver->Open            = _Open;
    driver->OpenAt          = _OpenAt;
    driver->Close           = _Close;
    driver->Stat            = _Stat;
    driver->Read            = _Read;
    driver->Write           = _Write;
    driver->Seek            = _Seek;
    driver->ReadDir         = _ReadDir;
    return 0;
}

void VfsWantedDestroy(vfs_driver_t *driver)
{
    WantedFree(driver->ctx);
    driver->ctx = NULL;
}


static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    return _OpenAt(d, 0, path, flags);
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    char normalized[MAX_PATH_LEN];
    const char *pathLeft;
    int f;

    DEBUG_TRACE("%d: %s (0x%x)", fd, path, flags);

    if (!CheckFd(d, fd)) { return -EBADF; }

    if (NULL == path || *path == '\0') {
        return -EINVAL;
    }

    if (cwk_path_normalize(path, normalized, MAX_PATH_LEN) >= MAX_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    f = VfsFindEntry(normalized, files, &pathLeft);
    if (f < 0) return f;

    if (!pathLeft) {
        pathLeft = "/";
    }

    if (f == FILES_CNT-1) { // last entry is directory of the registry, TODO: this is a little dirty w/a
        fd = TRY_DRV(d->fsDrv, Open, pathLeft, flags);
        if (fd < 0) { return fd; }
        fd += FILES_CNT;
    } else {
        fd = f;
    }

    return fd;
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
        return TRY_DRV(d->fsDrv, Close, fd-FILES_CNT);
    }

    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    if (NULL == stat) {
        return -EINVAL;
    }

    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
        return TRY_DRV(d->fsDrv, Stat, fd-FILES_CNT, stat);
    }

    stat->dev = *(uint32_t *)id;
    stat->ino = fd;
    stat->filetype = files[fd].type;
    stat->size = 0;
    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->oflags = 0;

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
        return TRY_DRV(d->fsDrv, Read, fd-FILES_CNT, buf, nbyte);
    }

    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
        return TRY_DRV(d->fsDrv, Write, fd-FILES_CNT, buf, nbyte);
    }

    return 0;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos)
{
    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
        return TRY_DRV(d->fsDrv, Seek, fd-FILES_CNT, off, whence, pos);
    }

    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    vfs_dirent_t dir;
    size_t used = 0;
    int f;

    DEBUG_TRACE("%d %d %lld", fd, bufLen, cookie);

    if (NULL == buf || NULL == cookie || NULL == bufUsed) { return -EINVAL; }
    if (!CheckFd(d, fd)) { return -EBADF; }

    if (fd >= FILES_CNT) {
         return TRY_DRV(d->fsDrv, ReadDir, fd-FILES_CNT, buf, bufLen, cookie, bufUsed);
    }

    if (*cookie == 0) {
        (*cookie)++;    // skip root entry
    }

    for (int i = *cookie; i < FILES_CNT; i++) {
        dir.d_ino       = i-1;
        dir.d_namlen    = strnlen(files[i].name, MAX_PATH_LEN);
        dir.d_type      = files[i].type;
        dir.d_next      = i;

        if (used + sizeof(dir) + dir.d_namlen > bufLen) {
            used = bufLen;
            break;
        }
        memcpy(buf + used, &dir, sizeof(dir));
        memcpy(buf + sizeof(dir) + used, files[i].name, dir.d_namlen);

        used += sizeof(dir) + dir.d_namlen;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}

