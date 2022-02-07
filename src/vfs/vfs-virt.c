#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "vfs-internal.h"
#include <vfs.h>
#include <cwalk.h>

#include <debug_trace.h>


/*
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

*/
