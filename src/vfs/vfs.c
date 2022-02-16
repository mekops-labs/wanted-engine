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

    return 0;
}

int VfsFdStat(vfs_ctx_t c, int fd, vfs_fdstat_t *stat)
{

    return 0;
}

int VfsFileStatAt(vfs_ctx_t c, int fd, const char *path, vfs_filestat_t *stat)
{

    return 0;
}

int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte)
{

    return 0;
}

int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte)
{

    return 0;
}

int VfsSeek(vfs_ctx_t c, int fd, long off, int whence, long *pos)
{

    return 0;
}

int VfsTell(vfs_ctx_t c, int fd, long *pos)
{

    return 0;
}

int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    return 0;
}
