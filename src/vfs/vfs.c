#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <wanted_malloc.h>
#include <vfs.h>
#include "vfs-internal.h"
#include <debug_trace.h>

#include <cwalk.h>

static inline
bool CheckFd(struct vfs_ctx_t *c, int fd)
{
    if (fd >= MAX_OPEN || fd < 0) return false;
    if (c && c->fildes[fd].drv == NULL) return false;

    return true;
}

static
int FindFirstClosedFd(struct vfs_ctx_t *c)
{
    if (!c) return -EINVAL;

    for (int i = ROOT_FD; i < MAX_OPEN; i++) {
        if (!c->fildes[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}

/* PUBLIC INTERFACE */

vfs_ctx_t VfsInit()
{
    struct vfs_ctx_t *c;

    c = (struct vfs_ctx_t *)WantedMalloc(sizeof(*c));
    if (!c) return c;

    memset(c, 0, sizeof(*c));

    return c;
}

static void DestroyDrivers(vfs_ctx_t c) {
    for (int i = 0; i < c->nDrivers; i++) {
        if (c->drivers[i].Destroy != NULL) c->drivers[i].Destroy(&c->drivers[i].ctx);
    }
    c->nDrivers = 0;
}

void VfsDestroy(vfs_ctx_t *c)
{
    if (NULL == c || NULL == *c) return;
    DestroyDrivers(*c);
    WantedFree(*c);
    *c = NULL;
}

int VfsRegister(vfs_ctx_t c, const char *path, const vfs_driver_t *driver)
{
    int ret = 0;
    DEBUG_TRACE("%s (%.4s)", path, driver->id);

    if (c->nDrivers >= MAX_DRIVERS) {
        return -ENOSPC;
    }

    if (NULL == driver || NULL == c) {
        return -EINVAL;
    }

    memcpy(&(c->drivers[c->nDrivers]), driver, sizeof(vfs_driver_t));

    driver = &(c->drivers[c->nDrivers]);

    struct cwk_segment seg;

    if (memcmp("/", path, 2) == 0) {
        c->fildes[ROOT_FD].drv = driver;
        ret = TRY_DRV(c->fildes[ROOT_FD].drv, Register, path, driver);
        if (ret < 0) return ret;
    } else if (memcmp("<stdin>", path, 8) == 0) {
        c->fildes[VFS_STDIN].drv = driver;
        c->fildes[VFS_STDIN].drv_fd = VFS_STDIN;
        c->fildes[VFS_STDIN].opened = true;
    } else if (memcmp("<stdout>", path, 9) == 0) {
        c->fildes[VFS_STDOUT].drv = driver;
        c->fildes[VFS_STDOUT].drv_fd = VFS_STDOUT;
        c->fildes[VFS_STDOUT].opened = true;
    } else if (memcmp("<stderr>", path, 9) == 0) {
        c->fildes[VFS_STDERR].drv = driver;
        c->fildes[VFS_STDERR].drv_fd = VFS_STDERR;
        c->fildes[VFS_STDERR].opened = true;
    } else {
        if (!c->fildes[ROOT_FD].drv) {
            return -EINVAL;
        }

        if (!cwk_path_get_first_segment(path, &seg)) {
            return -EINVAL;
        }

        ret = TRY_DRV(c->fildes[ROOT_FD].drv, Register, seg.begin, driver);
        if (ret < 0) return ret;
    }

    c->nDrivers++;

    return ret;
}

int VfsOpen(vfs_ctx_t c, const char *path, vfs_oflags_t flags)
{
    DEBUG_TRACE("%s (0x%x)", path, flags);

    return VfsOpenAt(c, ROOT_FD, path, flags);
}

int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, vfs_oflags_t flags)
{
    DEBUG_TRACE("%d, %s (0x%x)", fd, path, flags);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == path || *path == '\0') {
        return -EINVAL;
    }

    int f = FindFirstClosedFd(c);
    if (f < 0) return f;

    int newFd = TRY_DRV(c->fildes[fd].drv, OpenAt, c->fildes[fd].drv_fd, path, flags);
    if (newFd < 0) { return newFd; }

    c->fildes[f].drv    = c->fildes[fd].drv;
    c->fildes[f].drv_fd = newFd;
    c->fildes[f].flags  = flags;
    c->fildes[f].opened = true;

    return f;
}

int VfsClose(vfs_ctx_t c, int fd)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd)) return -EBADF;

    c->fildes[fd].opened = false;

    return TRY_DRV(c->fildes[fd].drv, Close, c->fildes[fd].drv_fd);
}

int VfsStatAt(vfs_ctx_t c, int fd, const char *path, vfs_stat_t *stat)
{
    int ret, f;

    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd)) return -EBADF;

    f = TRY_DRV(c->fildes[fd].drv, OpenAt, c->fildes[fd].drv_fd, path, 0);
    if (f < 0) { return f; }

    ret = TRY_DRV(c->fildes[fd].drv, Stat, f, stat);
    if (ret < 0) { return ret; }

    ret = TRY_DRV(c->fildes[fd].drv, Close, f);

    return ret;
}

int VfsStat(vfs_ctx_t c, int fd, vfs_stat_t *stat)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd)) return -EBADF;

    return TRY_DRV(c->fildes[fd].drv, Stat, c->fildes[fd].drv_fd, stat);
}

int VfsStatSet(vfs_ctx_t c, int fd, vfs_stat_t stat)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd)) return -EBADF;

    return TRY_DRV(c->fildes[fd].drv, StatSet, c->fildes[fd].drv_fd, stat);
}

int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d, %d", fd, nbyte);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == buf) {
        return -EINVAL;
    }

    return TRY_DRV(c->fildes[fd].drv, Read, c->fildes[fd].drv_fd, buf, nbyte);
}

int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d, %d", fd, nbyte);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == buf) {
        return -EINVAL;
    }

    return TRY_DRV(c->fildes[fd].drv, Write, c->fildes[fd].drv_fd, buf, nbyte);
}

int VfsSeek(vfs_ctx_t c, int fd, long off, vfs_whence_t whence, long *pos)
{
    DEBUG_TRACE("%d, %d, %d", fd, off, whence);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == pos) {
        return -EINVAL;
    }

    return TRY_DRV(c->fildes[fd].drv, Seek, c->fildes[fd].drv_fd, off, whence, pos);
}

int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    DEBUG_TRACE("%d (%d) %d", fd, bufLen, *cookie);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == buf || NULL == cookie || NULL == bufUsed) {
        return -EINVAL;
    }

    return TRY_DRV(c->fildes[fd].drv, ReadDir, c->fildes[fd].drv_fd, buf, bufLen, cookie, bufUsed);
}

int VfsUnlink(vfs_ctx_t c, int fd, const char *path)
{
    DEBUG_TRACE("%d (%s)", fd, path);

    if (!CheckFd(c, fd)) return -EBADF;

    if (NULL == path) {
        return -EINVAL;
    }

    return TRY_DRV(c->fildes[fd].drv, Unlink, c->fildes[fd].drv_fd, path);
}

int  VfsSockAccept  (vfs_ctx_t c, int fd, vfs_oflags_t flags, int *newFd)
{
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd)) {return -EBADF; }

    return TRY_DRV(c->fildes[fd].drv, SockAccept, c->fildes[fd].drv_fd, flags, newFd);
}

int  VfsSockRecv    (vfs_ctx_t c, int fd, void *buf, size_t nbyte, vfs_riflags_t iflags, vfs_roflags_t *oflags)
{
    DEBUG_TRACE("%d (0x%x) %d", fd, iflags, nbyte);

    if (!CheckFd(c, fd)) {return -EBADF; }

    return TRY_DRV(c->fildes[fd].drv, SockRecv, c->fildes[fd].drv_fd, buf, nbyte, iflags, oflags);
}

int  VfsSockSend    (vfs_ctx_t c, int fd, const void *buf, size_t nbyte, vfs_sdflags_t flags)
{
    DEBUG_TRACE("%d (0x%x) %d", fd, flags, nbyte);

    if (!CheckFd(c, fd)) {return -EBADF; }

    return TRY_DRV(c->fildes[fd].drv, SockSend, c->fildes[fd].drv_fd, buf, nbyte, flags);
}

int  VfsSockShutdown(vfs_ctx_t c, int fd, vfs_sdflags_t flags)
{
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd)) {return -EBADF; }

    return TRY_DRV(c->fildes[fd].drv, SockShutdown, c->fildes[fd].drv_fd, flags);
}
