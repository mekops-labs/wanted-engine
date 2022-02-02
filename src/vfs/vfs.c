#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <vfs.h>

#include "../wanted-internal.h"

#define MAX_OPEN 20

typedef struct vfs_entry_t {
    int             drv_fd;
    const char*     path;
    const char*     drv_path;
    vfs_driver_t    *drv;
    bool            opened;
} vfs_entry_t;

extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;
extern vfs_driver_t vfs_virtual_drv;

vfs_entry_t fildes[MAX_OPEN] = {
    { 0,  "<stdin>",  "",  &vfs_linux_drv,      true},
    { 1,  "<stdout>", "",  &vfs_linux_drv,      true},
    { 2,  "<stderr>", "",  &vfs_linux_drv,      true},
    { -1, "/",        "/", &vfs_virtual_drv,    false},
    { -1, "/dir",     ".", &vfs_linux_drv,      false},
    { -1, "/rom",     "/", &vfs_romfs_drv,      false},
};

static inline
bool CheckFd(int fd)
{
    if (fd >= MAX_OPEN) return false;
    if (fildes[fd].drv == NULL) return false;

    return true;
}

static
int FindFirstClosedFd()
{
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!fildes[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}

int VfsRegister(const char *path, const char *drvPath, vfs_driver_t *driver)
{
    return -1;
}

/* works only for preopened files */
int VfsOpen(const char *path, int flags)
{
    DEBUG_TRACE("%s", path);

    for (int i = 0; i < MAX_OPEN; i++) {
        if (!fildes[i].path) break;
        if (memcmp(fildes[i].path, path, strlen(fildes[i].path)+1) == 0) {
            if (fildes[i].drv_fd < 0) {
                fildes[i].drv_fd = fildes[i].drv->Open(fildes[i].drv_path, flags);
                fildes[i].opened = true;
            }
            return i;
        }
    }

    return -ENOENT;
}

int VfsOpenAt(int fd, const char *path, int flags)
{
    DEBUG_TRACE("%d: %s", fd, path);

    if (!CheckFd(fd)) return -EBADF;

    int new_fd = FindFirstClosedFd();
    if (new_fd < 0) return new_fd;

    fildes[new_fd].drv_fd = fildes[fd].drv->OpenAt(fildes[fd].drv_fd, path, flags);
    fildes[new_fd].drv = fildes[fd].drv;
    fildes[new_fd].opened = true;

    return new_fd;
}

int VfsClose(int fd)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    int ret = fildes[fd].drv->Close(fildes[fd].drv_fd);
    if (ret < 0) return ret;

    fildes[fd].opened = false;
    return ret;
}

int VfsFdStat(int fd, vfs_fdstat_t *stat)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->FdStat(fildes[fd].drv_fd, stat);
}

int VfsFileStatAt(int fd, const char *path, vfs_filestat_t *stat)
{
    DEBUG_TRACE("%d: %s", fd, path);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->FileStatAt(fildes[fd].drv_fd, path, stat);
}

int VfsRead(int fd, void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->Read(fildes[fd].drv_fd, buf, nbyte);;
}

int VfsWrite(int fd, const void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->Write(fildes[fd].drv_fd, buf, nbyte);
}

int VfsSeek(int fd, long off, int whence, long *pos)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->Seek(fildes[fd].drv_fd, off, whence, pos);
}

int VfsTell(int fd, long *pos)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

     return fildes[fd].drv->Tell(fildes[fd].drv_fd, pos);
}

int VfsReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    return fildes[fd].drv->ReadDir(fildes[fd].drv_fd, buf, bufLen, cookie, bufUsed);
}
