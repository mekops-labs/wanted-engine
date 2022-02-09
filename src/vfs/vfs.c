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

extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;
extern vfs_driver_t vfs_dummy_drv;

#define ROOT_FD     3

vfs_entry_t fildes[MAX_OPEN] = {
    { 0,        &vfs_linux_drv,      true},
    { 1,        &vfs_linux_drv,      true},
    { 2,        &vfs_linux_drv,      true},
};

file_t root[] = {
    {"/",    0, NULL,          },
    {"dev",  1, NULL,          },
    {"xyz",  2, &vfs_dummy_drv,},
    {"dir",  1, &vfs_linux_drv,},
    {"net",  1, NULL,          },
    {"sock", 2, NULL,          },
    {"rom",  1, &vfs_romfs_drv,},
    {"sys",  1, NULL,          },
    {"bus",  2, NULL,          },
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

/* requires normalized path */
int VfsFindEntryAt(int fd, const char *path, file_t *files, size_t filesCnt, const char **pathLeft)
{
    struct cwk_segment seg;
    int f;
    uint16_t d;
    bool found = false;

    if (pathLeft) {
        *pathLeft = NULL;
    }

    if (fd >= filesCnt) {
        return -EBADF;
    }

    if (files[fd].drv && files[fd].drv->filetype != VFS_FILETYPE_DIRECTORY) {
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
            if ((files[f].depth == d) && strncmp(files[f].name, seg.begin, MAX(seg.size, strlen(files[f].name))) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

        d++;
        fd = f;
    } while (files[f].drv == NULL && cwk_path_get_next_segment(&seg));

    if (!found) {
        return -ENOENT;
    }

    if (pathLeft) {
        if (files[f].drv == NULL) {
            *pathLeft = NULL;
        } else {
            if (!cwk_path_get_next_segment(&seg)) {
                *pathLeft = ".";
            } else {
                *pathLeft = seg.begin;
            }

            DEBUG_TRACE("pathLeft: %s", *pathLeft);
        }
    }

    return f;
}


/* PUBLIC INTERFACE */

int VfsRegister(const char *path, const char *drvPath, vfs_driver_t *driver)
{
    return -1;
}

int VfsOpen(const char *path, int flags)
{
    DEBUG_TRACE("%s (0x%x)", path, flags);

    return VfsOpenAt(ROOT_FD, path, flags);
}

int VfsOpenAt(int fd, const char *path, int flags)
{
    char normalized[MAX_PATH_LEN];
    const char *pathLeft;
    DEBUG_TRACE("%d: %s (0x%x)", fd, path, flags);

    if (fd < ROOT_FD) {
        return fd;
    }

    if (!CheckFd(fd)) return -EBADF;

    int new_fd = FindFirstClosedFd();
    if (new_fd < 0) return new_fd;

    if (cwk_path_normalize(path, normalized, MAX_PATH_LEN) >= MAX_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    int f = VfsFindEntryAt(fd - ROOT_FD, normalized, root, rootLen, &pathLeft);
    if (f < 0) return f;

    if (pathLeft && NULL != root[f].drv) {
        fildes[new_fd].drv = root[f].drv;
        int rootFd = TRY(fildes[new_fd].drv, Open, "/", 0);
        f = TRY(fildes[new_fd].drv, OpenAt, rootFd, pathLeft, flags);
        TRY(fildes[new_fd].drv, Close, rootFd);
        if (f < 0) return f;
    } else {
        fildes[new_fd].drv = NULL;
    }

    fildes[new_fd].drv_fd = f;
    fildes[new_fd].opened = true;

    pathLeft = path;

    return new_fd;
}

int VfsClose(int fd)
{
    int ret = 0;
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    if (NULL != fildes[fd].drv) {
        ret = TRY(fildes[fd].drv, Close, fildes[fd].drv_fd);
    }

    fildes[fd].opened = false;
    fildes[fd].drv = NULL;
    return ret;
}

int VfsFdStat(int fd, vfs_fdstat_t *stat)
{
    int ret = 0;
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    stat->filetype = root[fd].drv ? root[fd].drv->filetype : VFS_FILETYPE_DIRECTORY;
    stat->flags = 0;

    if (NULL != fildes[fd].drv) {
        ret = TRY(fildes[fd].drv, FdStat, fd, stat);
    }

    return ret;
}

int VfsFileStatAt(int fd, const char *path, vfs_filestat_t *stat)
{
    int f, ret = 0;
    char normalized[MAX_PATH_LEN];
    const char *pathLeft;
    const char drvRoot[] = {'/', '\0'};

    DEBUG_TRACE("%d: %s", fd, path);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    fd = fildes[fd].drv_fd;

    if (cwk_path_normalize(path, normalized, MAX_PATH_LEN) >= MAX_PATH_LEN) {
        return -ENAMETOOLONG;
    }

    f = VfsFindEntryAt(fd, normalized, root, rootLen, &pathLeft);
    if (f < 0) return f;

    if (pathLeft && NULL != root[f].drv) {
        fd = TRY(root[f].drv, Open, drvRoot, 0);
        ret = TRY(root[f].drv, FileStatAt, fd, pathLeft, stat);
        TRY(root[f].drv, Close, fd);
        if (ret < 0) return ret;
    } else {
        stat->atim = 0;
        stat->ctim = 0;
        stat->mtim = 0;
        stat->dev = (fildes[f].drv != NULL) ? *(uint32_t*)fildes[f].drv->id : 0;
        stat->ino = f;
        stat->filetype = root[f].drv ? root[f].drv->filetype : VFS_FILETYPE_DIRECTORY;
        stat->nlink = 0;
        stat->size = 0;
    }

    return 0;
}

int VfsRead(int fd, void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    if (NULL == fildes[fd].drv) {
        return -EPERM;
    }

    return TRY(fildes[fd].drv, Read, fildes[fd].drv_fd, buf, nbyte);
}

int VfsWrite(int fd, const void *buf, size_t nbyte)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    if (NULL == fildes[fd].drv) {
        return -EPERM;
    }

    return TRY(fildes[fd].drv, Write ,fildes[fd].drv_fd, buf, nbyte);
}

int VfsSeek(int fd, long off, int whence, long *pos)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    if (NULL == fildes[fd].drv) {
        return -EPERM;
    }

    return TRY(fildes[fd].drv, Seek, fildes[fd].drv_fd, off, whence, pos);
}

int VfsTell(int fd, long *pos)
{
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    if (NULL == fildes[fd].drv) {
        return -EPERM;
    }

    return TRY(fildes[fd].drv, Tell, fildes[fd].drv_fd, pos);
}

int VfsReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    vfs_dirent_t dir;
    size_t used = 0;
    int f;
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(fd)) return -EBADF;

    if (!fildes[fd].opened) {
        return -EBADF;
    }

    f = fildes[fd].drv_fd;

    if (NULL != fildes[fd].drv) {
        return TRY(fildes[fd].drv, ReadDir, f, buf, bufLen, cookie, bufUsed);
    }

    for (int i = f + 1; (i < rootLen) && (root[i].depth > root[f].depth); i++) {
        if (root[i].depth == root[f].depth + 1) {
            dir.d_ino       = i;
            dir.d_namlen    = strnlen(root[i].name, 256);
            dir.d_type      = root[i].drv ? root[i].drv->filetype : VFS_FILETYPE_DIRECTORY;
            dir.d_next      = i;

            if (used + sizeof(dir) + dir.d_namlen > bufLen) {
                used = bufLen;
                break;
            }
            memcpy(buf + used, &dir, sizeof(dir));
            memcpy(buf + sizeof(dir) + used, root[i].name, dir.d_namlen);

            used += sizeof(dir) + dir.d_namlen;
        }
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}
