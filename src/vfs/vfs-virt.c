#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "vfs-internal.h"
#include <vfs.h>
#include <cwalk.h>

#include <debug_trace.h>


extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;

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

static int  _Open(const char *path, int flags);
static int  _OpenAt(int fd, const char *path, int flags);
static int  _Close(int fd);
static int  _FdStat(int fd, vfs_fdstat_t *stat);
static int  _FileStatAt(int fd, const char *path, vfs_filestat_t *stat);
static int  _Read(int fd, void *buf, size_t nbyte);
static int  _Write(int fd, const void *buf, size_t nbyte);
static int  _Seek(int fd, long off, int whence, long *pos);
static int  _Tell(int fd, long *pos);
static int  _ReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);

vfs_driver_t vfs_virtual_drv = {
    .id         = { 'V', 'i', 'r', 't' },
    .Open       = _Open,
    .OpenAt     = _OpenAt,
    .Close      = _Close,
    .FdStat     = _FdStat,
    .FileStatAt = _FileStatAt,
    .Read       = _Read,
    .Write      = _Write,
    .Seek       = _Seek,
    .Tell       = _Tell,
    .ReadDir    = _ReadDir,
};

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

        for (f = fd+1; (f < filesCnt); f++) {
            if ((files[f].depth == d) && strncmp(files[f].name, seg.begin, seg.size) == 0) {
                found = true;
                break;
            }
        }
        if (!found) break;

        d++;
    } while (cwk_path_get_next_segment(&seg));

    if (found) {
        return f;
    } else {
        return -ENOENT;
    }
}

static int _Open(const char *path, int flags)
{
    return _OpenAt(0, path, flags);
}

static int _OpenAt(int fd, const char *path, int flags)
{
    return VfsFindFileAt(fd, path, root, rootLen);
}

static int _Close(int fd)
{
    return 0;
}

static int _FdStat(int fd, vfs_fdstat_t *s)
{
    if (fd < rootLen) {
        s->filetype = root[fd].type;
    } else {
        return -EBADF;
    }

    return 0;
}

static int _FileStatAt(int fd, const char *path, vfs_filestat_t *s)
{
    int f = VfsFindFileAt(fd, path, root, rootLen);
    if (f < 0) return f;

    s->atim = 0;
    s->ctim = 0;
    s->mtim = 0;
    s->dev = (root[f].driver != NULL) ? *(uint32_t*)root[f].driver->id : 0;
    s->ino = f;
    s->filetype = root[f].type;
    s->nlink = 0;
    s->size = 0;

    return 0;
}

static int _Read(int fd, void *buf, size_t nbyte)
{
    static bool read = false;
    char c[2];

    if (fd >= rootLen) {
        return -EBADF;
    }

    if (root[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    if (nbyte < 2) {
        return -EINVAL;
    }

    c[0] = (root[fd].driver != NULL) ? *(uint32_t*)root[fd].driver->id : 0; + 0x30;
    c[1] = '\n';

    memcpy(buf, &c, 2);

    if (read) {
        read = false;
        return 0;
    } else {
        read = true;
        return 2;
    }
}

static int  _Write(int fd, const void *buf, size_t nbyte)
{
    return -EROFS;
}

static int _Seek(int fd, long off, int whence, long *pos)
{
    return 0;
}

static int _Tell(int fd, long *pos)
{
    return 0;
}

static int _ReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    vfs_dirent_t dir;
    size_t used = 0;

    if (fd >= rootLen) {
        return -EBADF;
    }

    if (root[fd].type != VFS_FILETYPE_DIRECTORY) {
        return -ENOTDIR;
    }

    for (int i = fd + 1; i < rootLen && (root[i].depth > root[fd].depth); i++) {
        if (root[i].depth == root[fd].depth + 1) {
            dir.d_ino       = i;
            dir.d_namlen    = strnlen(root[i].name, 256);
            dir.d_type      = root[i].type;
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
