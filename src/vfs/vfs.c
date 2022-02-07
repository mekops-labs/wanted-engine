#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>

#include <vfs.h>
#include "vfs-internal.h"
#include <debug_trace.h>

#include <cwalk.h>

#define MAX_OPEN 20

typedef struct vfs_entry_t {
    int             drv_fd;
    vfs_driver_t    *drv;
    bool            opened;
} vfs_entry_t;

extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;

#define ROOT_FD     3

vfs_entry_t fildes[MAX_OPEN] = {
    { 0,        &vfs_linux_drv,      true},
    { 1,        &vfs_linux_drv,      true},
    { 2,        &vfs_linux_drv,      true},
    { ROOT_FD,  NULL,          true},
};

file_t root[] = {
    {"/",    0, VFS_FILETYPE_DIRECTORY,         NULL,          },
    {"dev",  1, VFS_FILETYPE_DIRECTORY,         NULL,          },
    {"xyz",  2, VFS_FILETYPE_CHARACTER_DEVICE,  NULL,          },
    {"dir",  1, VFS_FILETYPE_DIRECTORY,         &vfs_linux_drv,},
    {"net",  1, VFS_FILETYPE_DIRECTORY,         NULL,          },
    {"sock", 2, VFS_FILETYPE_SOCKET_STREAM,     NULL,          },
    {"rom",  1, VFS_FILETYPE_DIRECTORY,         &vfs_romfs_drv,},
    {"sys",  1, VFS_FILETYPE_DIRECTORY,         NULL,          },
    {"bus",  2, VFS_FILETYPE_SOCKET_DGRAM,      NULL,          },
};

const size_t rootLen = sizeof(root)/sizeof(root[0]);

static inline
bool CheckFd(int fd)
{
    if (fd >= MAX_OPEN) return false;
    //if (fildes[fd].drv == NULL) return false;

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

int VfsFindFileAt(int fd, const char *path, file_t *files, size_t filesCnt)
{
    struct cwk_segment seg;
    int f;
    uint16_t d;
    bool found = false;

    if (fd >= filesCnt) {
        return -EBADF;
    }

    if (files[fd].type != VFS_FILETYPE_DIRECTORY) {
        return -ENOTDIR;
    }

    d = files[fd].depth + 1;
    f = fd;

    if (cwk_path_is_absolute(path)) {
        while (*path == '/') {
            path++;
        }
        fd = 0;
    }

    DEBUG_TRACE("path: %s", path);

    cwk_path_get_first_segment(path, &seg);

    if (seg.size == 0) {
        // probably could only happen when initial path was /
        return fd;
    }

    do {
        DEBUG_TRACE("segment: %.*s (%d)", seg.size, seg.begin, seg.size);
        found = false;


        if (memcmp(".", seg.begin, seg.size) == 0) {
            found = true;
            continue;
        }

        if (memcmp("..", seg.begin, seg.size) == 0) {
            if (d == 1) break; // root dir can't go up
            found = true;
            f = fd = 0;
            d--;
            continue;
        }

        for (f = fd+1; (f < filesCnt) && (files[f].depth > files[fd].depth); f++) {
            if ((files[f].depth == d) && strncmp(files[f].name, seg.begin, seg.size) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

        d++;
        fd = f;
    } while (cwk_path_get_next_segment(&seg));

    if (found) {
        return f;
    } else {
        return -ENOENT;
    }
}


/* PUBLIC INTERFACE */

int VfsRegister(const char *path, const char *drvPath, vfs_driver_t *driver)
{
    return -1;
}

int VfsOpen(const char *path, int flags)
{
    DEBUG_TRACE("%s (0x%x)", path, flags);

    return VfsOpenAt(0, path, flags);
}

int VfsOpenAt(int fd, const char *path, int flags)
{
    DEBUG_TRACE("%d: %s (0x%x)", fd, path, flags);

    if (!CheckFd(fd)) return -EBADF;

    int new_fd = FindFirstClosedFd();
    if (new_fd < 0) return new_fd;

    fd = fd - ROOT_FD;

    int f = VfsFindFileAt(fd, path, root, rootLen);
    if (f < 0) return f;
    fildes[new_fd].drv_fd = f;

    TRY(fildes[f].drv, OpenAt, fildes[f].drv_fd, path, flags);

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
