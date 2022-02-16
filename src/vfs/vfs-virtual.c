#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <wanted_malloc.h>
#include "vfs-internal.h"

#include <vfs.h>
#include <vfs-drivers.h>
#include <debug_trace.h>
#include <cwalk.h>

#include "vfs-virtual.h"

#include <assert.h>

const char id[] = { 'V', 'i', 'r', 't' };

static int _Open(vfs_driver_ctx_t d, const char *path, int flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, int flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _FdStat(vfs_driver_ctx_t d, int fd, vfs_fdstat_t *stat);
static int _FileStatAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_filestat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, int whence, long *pos);
static int _Tell(vfs_driver_ctx_t d, int fd, long *pos);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);
static int _Register(vfs_driver_ctx_t d, const char *path, vfs_driver_t *driver);

static inline
bool CheckFd(vfs_driver_ctx_t d, int fd)
{
    if (fd >= MAX_OPEN) return false;
    if (d && d->fildes[fd].drv == NULL) return false;

    return true;
}

static inline
bool CheckSameDriver(vfs_driver_ctx_t d, int fd)
{
    return (d->fildes[fd].drv->ctx == d);
}

static inline
bool CheckOpened(vfs_driver_ctx_t d, int fd)
{
    return CheckFd(d, fd) && d->fildes[fd].opened;
}

static
int FindFirstClosedFd(vfs_driver_ctx_t d)
{
    if (!d) return -EINVAL;

    for (int i = 0; i < MAX_OPEN; i++) {
        if (!d->fildes[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}


int VfsVirtualInit(vfs_driver_t *driver)
{
    int ret;

    if (NULL == driver) {
        return -EINVAL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) return -ENOMEM;

    memset(driver->ctx, 0, sizeof(struct vfs_driver_ctx_t));

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = VFS_FILETYPE_DIRECTORY;
    driver->Register        = _Register;
    driver->Open            = _Open;
    driver->OpenAt          = _OpenAt;
    driver->Close           = _Close;
    driver->FdStat          = _FdStat;
    driver->FileStatAt      = _FileStatAt;
    driver->Read            = _Read;
    driver->Write           = _Write;
    driver->Seek            = _Seek;
    driver->Tell            = _Tell;
    driver->ReadDir         = _ReadDir;

    driver->ctx->entries[0].drv         = driver;
    driver->ctx->entries[0].name[0]     = '/';
    driver->ctx->cnt++;

    driver->ctx->fildes[0].drv_fd       = 0;
    driver->ctx->fildes[0].drv          = driver;
    driver->ctx->fildes[0].opened       = true;
    driver->ctx->fildes[0].flags        = VFS_O_RDONLY;

    return 0;
}

void VfsVirtualDestroy(vfs_driver_t *driver)
{
    WantedFree(driver->ctx);
    driver->ctx = NULL;
}

int VfsFindEntry(const char *path, vfs_entry_t *files, const char **pathLeft)
{
    struct cwk_segment seg;
    int f = 0;
    bool found = false;

    if (pathLeft) {
        *pathLeft = NULL;
    }

    if (!cwk_path_get_first_segment(path, &seg)) {
        return 0;
    }

    do {
        DEBUG_TRACE("segment: %.*s (%d)", seg.size, seg.begin, seg.size);
        found = false;

        if (memcmp(".", seg.begin, seg.size) == 0) {
            found = true;
            continue;
        }

        for (f = 0; files[f].name[0] != '\0'; f++) {
            if (strncmp(files[f].name, seg.begin, MAX(seg.size, strlen(files[f].name))) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

    } while (files[f].drv == NULL && cwk_path_get_next_segment(&seg));

    if (!found) {
        return -ENOENT;
    }

    if (cwk_path_get_next_segment(&seg)) {
        if (pathLeft) {
            if (files[f].drv == NULL) {
                *pathLeft = NULL;
            } else {
                *pathLeft = seg.begin;
            }
            DEBUG_TRACE("pathLeft: %s", *pathLeft);
        } else {
            f = -ENOENT;
        }
    }

    return f;
}

static int _Register(vfs_driver_ctx_t d, const char *path, vfs_driver_t *driver) {
    const char *pathLeft;
    struct cwk_segment seg;

    if (memcmp(".", path, 2) == 0 || memcmp("..", path, 3) == 0) {
        return -EINVAL;
    }

    if (!cwk_path_get_first_segment(path, &seg)) {
        return -EINVAL;
    }

    int entry = VfsFindEntry(seg.begin, d->entries, &pathLeft);

    if (entry == -ENOENT) {
        if (d->cnt >= MAX_ENTRIES) {
            return -ENFILE;
        }
        entry = d->cnt++;
        d->entries[entry].drv = driver;
        memcpy(d->entries[entry].name, seg.begin, seg.size);
    } else if (pathLeft) {
        return TRY_DRV(d->entries[entry].drv, Register, pathLeft, driver);
    } else {
        d->entries[entry].drv = driver;
        memcpy(d->entries[entry].name, seg.begin, seg.size);
    }
    return entry;
}

static int _Open(vfs_driver_ctx_t d, const char *path, int flags)
{
    return _OpenAt(d, 0, path, flags);
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, int flags)
{
    char normalized[MAX_PATH_LEN];
    const char *pathLeft;
    int f, newFd;

    DEBUG_TRACE("%d: %s (0x%x)", fd, path, flags);

    if (!CheckFd(d, fd)) { return -EBADF; }

    if (NULL == path || *path == '\0') {
        return -EINVAL;
    }

    if (cwk_path_normalize(path, normalized, MAX_PATH_LEN) >= MAX_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    f = VfsFindEntry(normalized, d->entries, &pathLeft);
    if (f < 0) return f;

    fd = FindFirstClosedFd(d);
    if (fd < 0) return fd;

    assert(d->entries[f].drv != NULL);

    if (!pathLeft) {
        pathLeft = "/";
    }

    newFd = f;

    if (d->entries[f].drv->ctx != d) {
        newFd = TRY_DRV(d->entries[f].drv, Open, pathLeft, flags);
        if (newFd < 0) { return newFd; }
    }

    d->fildes[fd].drv_fd = newFd;
    d->fildes[fd].drv = d->entries[f].drv;
    d->fildes[fd].opened = true;
    d->fildes[fd].flags = flags;

    return fd;
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    int r;
    if (!CheckOpened(d, fd)) { return -EBADF; }

    if (CheckSameDriver(d, fd)) {
        if (fd == 0) { return -EBADF; }
        r = 0;
    } else {
        r = TRY_DRV(d->fildes[fd].drv, Close, d->fildes[fd].drv_fd);
    }

    d->fildes[fd].opened = false;
    return r;
}

static int _FdStat(vfs_driver_ctx_t d, int fd, vfs_fdstat_t *stat)
{
    if (!CheckOpened(d, fd)) { return -EBADF; }
    if (!stat) { return -EINVAL; }

    if (CheckSameDriver(d, fd)) {
        stat->filetype = d->fildes[fd].drv->filetype;
        stat->flags    = d->fildes[fd].flags;
        return 0;
    }

    return TRY_DRV(d->fildes[fd].drv, FdStat, d->fildes[fd].drv_fd, stat);
}

static int _FileStatAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_filestat_t *stat)
{
    int f, ret = 0;
    char normalized[MAX_PATH_LEN];
    const char *pathLeft;

    DEBUG_TRACE("%d: %s", fd, path);

    if (!CheckOpened(d, fd)) { return -EBADF; }

    if (NULL == path || *path == '\0' || NULL == stat) {
        return -EINVAL;
    }

    if (cwk_path_normalize(path, normalized, MAX_PATH_LEN) >= MAX_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    f = VfsFindEntry(normalized, d->entries, &pathLeft);
    if (f < 0) return f;

    if (pathLeft && NULL != d->entries[f].drv) {
        fd = TRY_DRV(d->entries[f].drv, Open, "/", 0);
        ret = TRY_DRV(d->entries[f].drv, FileStatAt, fd, pathLeft, stat);
        TRY_DRV(d->entries[f].drv, Close, fd);
        if (ret < 0) return ret;
    } else {
        stat->atim = 0;
        stat->ctim = 0;
        stat->mtim = 0;
        stat->dev = (d->entries[f].drv != NULL) ? d->entries[f].drv->bytesId : 0;
        stat->ino = f;
        stat->filetype = (d->entries[f].drv != NULL) ? d->entries[f].drv->filetype : VFS_FILETYPE_DIRECTORY;
        stat->nlink = 0;
        stat->size = 0;
    }

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    if (!CheckOpened(d, fd)) { return -EBADF; }
    if (NULL == buf) { return -EINVAL; }
    if (CheckSameDriver(d, fd)) { return -EISDIR; }

    return TRY_DRV(d->fildes[fd].drv, Read, d->fildes[fd].drv_fd, buf, nbyte);
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    if (!CheckOpened(d, fd)) { return -EBADF; }
    if (NULL == buf) { return -EINVAL; }
    if (CheckSameDriver(d, fd)) { return -EISDIR; }

    return TRY_DRV(d->fildes[fd].drv, Write, d->fildes[fd].drv_fd, buf, nbyte);
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, int whence, long *pos)
{
    if (!CheckOpened(d, fd)) { return -EBADF; }
    if (NULL == pos) { return -EINVAL; }
    if (whence > VFS_SEEK_END) { return -EINVAL; }

    if (CheckSameDriver(d, fd)) {
        return -EISDIR;
    }

    return TRY_DRV(d->fildes[fd].drv, Seek, d->fildes[fd].drv_fd, off, whence, pos);
}

static int _Tell(vfs_driver_ctx_t d, int fd, long *pos)
{
    if (!CheckOpened(d, fd)) { return -EBADF; }
    if (NULL == pos) { return -EINVAL; }

    if (CheckSameDriver(d, fd)) {
        return -EISDIR;
    }

    return TRY_DRV(d->fildes[fd].drv, Tell, d->fildes[fd].drv_fd, pos);
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    vfs_dirent_t dir;
    size_t used = 0;
    int f;

    DEBUG_TRACE("%d %d %lld", fd, bufLen, cookie);

    if (NULL == buf || NULL == cookie || NULL == bufUsed) { return -EINVAL; }
    if (!CheckOpened(d, fd)) { return -EBADF; }

    f = d->fildes[fd].drv_fd;

    if (!CheckSameDriver(d, fd)) {
         return TRY_DRV(d->fildes[fd].drv, ReadDir, f, buf, bufLen, cookie, bufUsed);
    }

    if (*cookie == 0) {
        // skip root entry
        *cookie = 1;
    }

    for (int i = *cookie; i < d->cnt; i++) {
        dir.d_ino       = i-1;
        dir.d_namlen    = strnlen(d->entries[i].name, MAX_PATH_LEN);
        dir.d_type      = d->entries[i].drv ? d->entries[i].drv->filetype : VFS_FILETYPE_UNKNOWN;
        dir.d_next      = i;

        if (used + sizeof(dir) + dir.d_namlen > bufLen) {
            used = bufLen;
            break;
        }
        memcpy(buf + used, &dir, sizeof(dir));
        memcpy(buf + sizeof(dir) + used, d->entries[i].name, dir.d_namlen);

        used += sizeof(dir) + dir.d_namlen;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}
