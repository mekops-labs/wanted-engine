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
    if (fd >= MAX_OPEN) return false;
    if (c && c->fildes[fd].drv == NULL) return false;

    return true;
}

static
int FindFirstClosedFd(struct vfs_ctx_t *c)
{
    if (!c) return -EINVAL;

    for (int i = 0; i < MAX_OPEN; i++) {
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
    memset(c, 0, sizeof(*c));

    return c;
}

void VfsDestroy(vfs_ctx_t *c)
{
    if (NULL == c || NULL == *c) return;
    WantedFree(*c);
    *c = NULL;
}

int VfsRegister(vfs_ctx_t c, const char *path, vfs_driver_t *driver)
{
    if (NULL == driver || NULL == c) {
        return -EINVAL;
    }

    struct cwk_segment seg;

    if (memcmp("/", path, 2) == 0) {
        c->fildes[ROOT_FD].drv = driver;
        c->fildes[ROOT_FD].drv_fd = TRY_DRV(driver, Open, "/", 0);
        if (c->fildes[ROOT_FD].drv_fd < 0) {
            return c->fildes[ROOT_FD].drv_fd;
        }
        c->fildes[ROOT_FD].opened = true;
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
        if (!c->fildes[ROOT_FD].opened) {
            return -EINVAL;
        }

        if (!cwk_path_get_first_segment(path, &seg)) {
            return -EINVAL;
        }

        return TRY_DRV(c->fildes[ROOT_FD].drv, Register, seg.begin, driver);
    }
    return 0;
}

int VfsOpen(vfs_ctx_t c, const char *path, int flags)
{
    DEBUG_TRACE("%s (0x%x)", path, flags);

    return VfsOpenAt(c, ROOT_FD, path, flags);
}

int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, int flags)
{
    if (!CheckFd(c, fd)) return -EBADF;
}

int VfsClose(vfs_ctx_t c, int fd)
{
    // int ret = 0;
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // if (NULL != fildes[fd].drv) {
    //     ret = TRY_DRV(fildes[fd].drv, Close, fildes[fd].drv_fd);
    // }

    // fildes[fd].opened = false;
    // fildes[fd].drv = NULL;
    // return ret;
    return 0;
}

int VfsFdStat(vfs_ctx_t c, int fd, vfs_fdstat_t *stat)
{
    // int ret = 0;
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // stat->filetype = root[fd].drv ? root[fd].drv->filetype : VFS_FILETYPE_DIRECTORY;
    // stat->flags = 0;

    // if (NULL != fildes[fd].drv) {
    //     ret = TRY_DRV(fildes[fd].drv, FdStat, fd, stat);
    // }

    // return ret;
    return 0;
}

int VfsFileStatAt(vfs_ctx_t c, int fd, const char *path, vfs_filestat_t *stat)
{

    return 0;
}

int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte)
{
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // if (NULL == fildes[fd].drv) {
    //     return -EPERM;
    // }

    // return TRY_DRV(fildes[fd].drv, Read, fildes[fd].drv_fd, buf, nbyte);
    return 0;
}

int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte)
{
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // if (NULL == fildes[fd].drv) {
    //     return -EPERM;
    // }

    // return TRY_DRV(fildes[fd].drv, Write ,fildes[fd].drv_fd, buf, nbyte);
    return 0;
}

int VfsSeek(vfs_ctx_t c, int fd, long off, int whence, long *pos)
{
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // if (NULL == fildes[fd].drv) {
    //     return -EPERM;
    // }

    // return TRY_DRV(fildes[fd].drv, Seek, fildes[fd].drv_fd, off, whence, pos);
    return 0;
}

int VfsTell(vfs_ctx_t c, int fd, long *pos)
{
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // if (NULL == fildes[fd].drv) {
    //     return -EPERM;
    // }

    // return TRY_DRV(fildes[fd].drv, Tell, fildes[fd].drv_fd, pos);
    return 0;
}

int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    // vfs_dirent_t dir;
    // size_t used = 0;
    // int f;
    // DEBUG_TRACE("%d", fd);

    // if (!CheckFd(fd)) return -EBADF;

    // if (!fildes[fd].opened) {
    //     return -EBADF;
    // }

    // f = fildes[fd].drv_fd;

    // if (NULL != fildes[fd].drv) {
    //     return TRY_DRV(fildes[fd].drv, ReadDir, f, buf, bufLen, cookie, bufUsed);
    // }

    // for (int i = f + 1; root[i].name != NULL; i++) {
    //     //if (root[i].depth == root[f].depth + 1) {
    //         dir.d_ino       = i;
    //         dir.d_namlen    = strnlen(root[i].name, 256);
    //         dir.d_type      = root[i].drv ? root[i].drv->filetype : VFS_FILETYPE_DIRECTORY;
    //         dir.d_next      = i;

    //         if (used + sizeof(dir) + dir.d_namlen > bufLen) {
    //             used = bufLen;
    //             break;
    //         }
    //         memcpy(buf + used, &dir, sizeof(dir));
    //         memcpy(buf + sizeof(dir) + used, root[i].name, dir.d_namlen);

    //         used += sizeof(dir) + dir.d_namlen;
    //     //}
    // }

    // *bufUsed = used;
    // *cookie = dir.d_next; // last found directory entry

    // return 0;
    return 0;
}
