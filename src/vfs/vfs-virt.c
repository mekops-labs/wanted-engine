#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "vfs-internal.h"
#include <vfs.h>
#include <cwalk.h>


file_t files[] = {
    {".",    0, VFS_FILETYPE_DIRECTORY,         -1},
    {"dev",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"xyz",  1, VFS_FILETYPE_CHARACTER_DEVICE,   1},
    {"dir",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"net",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"sock", 3, VFS_FILETYPE_SOCKET_STREAM,      2},
    {"rom",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"sys",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"bus",  5, VFS_FILETYPE_SOCKET_DGRAM,       0},
};

const size_t files_cnt = sizeof(files)/sizeof(files[0]);

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
    .id = { 'V', 'i', 'r', 't' },
    .Open        = _Open,
    .OpenAt      = _OpenAt,
    .Close       = _Close,
    .FdStat      = _FdStat,
    .FileStatAt  = _FileStatAt,
    .Read        = _Read,
    .Write       = _Write,
    .Seek        = _Seek,
    .Tell        = _Tell,
    .ReadDir     = _ReadDir,
};

int VfsFindFile(int fd, const char *path, file_t *files, size_t filesCnt)
{
    struct cwk_segment seg;
    char normalized[20];
    int last;

    if (fd >= filesCnt) return -EBADF;

    cwk_path_normalize(path, normalized, 20);

    if (strlen(normalized) == 1) {
         if (normalized[0] == '/') normalized[0] = '.';
    }

    if (!cwk_path_get_first_segment(normalized, &seg)) return -EINVAL;

    do {
        if (seg.size == 0) continue;
        last = -1;
        for (int i = fd; i < filesCnt; i++) {
            if (files[i].parent == fd && strlen(files[i].name) == seg.size && strncmp(files[i].name, seg.begin, seg.size) == 0) {
                last = i;
                fd = last;
                break;
            }
        }
    } while (cwk_path_get_next_segment(&seg) && last != -1);

    return last == -1 ? -ENOENT : last;
}

static int _Open(const char *path, int flags)
{
    return _OpenAt(0, path, flags);
}

static int _OpenAt(int fd, const char *path, int flags)
{
    return VfsFindFile(fd, path, files, files_cnt);
}

static int _Close(int fd)
{
    return 0;
}

static int _FdStat(int fd, vfs_fdstat_t *s)
{
    if (fd < files_cnt) {
        s->filetype = files[fd].type;
    } else {
        return -EBADF;
    }

    return 0;
}

static int _FileStatAt(int fd, const char *path, vfs_filestat_t *s)
{
    int f = VfsFindFile(fd, path, files, files_cnt);
    if (f < 0) return f;

    s->atim = 0;
    s->ctim = 0;
    s->mtim = 0;
    s->dev = files[f].driver;
    s->ino = f;
    s->filetype = files[f].type;
    s->nlink = 0;
    s->size = 0;

    return 0;
}

static int _Read(int fd, void *buf, size_t nbyte)
{
    static bool read = false;
    char c[2];

    if (fd >= files_cnt) {
        return -EBADF;
    }

    if (files[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    if (nbyte < 2) {
        return -EINVAL;
    }

    c[0] = files[fd].driver + 0x30;
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

    if (fd >= files_cnt) {
        return -EBADF;
    }

    if (files[fd].type != VFS_FILETYPE_DIRECTORY) {
        return -ENOTDIR;
    }

    for (int i = fd + 1; i < files_cnt; i++) {
        if (files[i].parent == fd) {
            dir.d_ino       = i;
            dir.d_namlen    = strnlen(files[i].name, 256);
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
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}
