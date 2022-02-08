#define _DEFAULT_SOURCE
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include <vfs.h>

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

vfs_driver_t vfs_linux_drv = {
    .id = { 'L', 'i', 'n', 'u' },
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

static inline vfs_filetype_t convertFiletype(uint32_t t)
{
    switch (t & S_IFMT) {
    case S_IFSOCK: return VFS_FILETYPE_SOCKET_STREAM;
    case S_IFLNK:  return VFS_FILETYPE_SYMBOLIC_LINK;
    case S_IFREG:  return VFS_FILETYPE_REGULAR_FILE;
    case S_IFBLK:  return VFS_FILETYPE_BLOCK_DEVICE;
    case S_IFDIR:  return VFS_FILETYPE_DIRECTORY;
    case S_IFCHR:  return VFS_FILETYPE_CHARACTER_DEVICE;
    case S_IFIFO:  return VFS_FILETYPE_REGULAR_FILE;
    default:       return VFS_FILETYPE_UNKNOWN;
    }
}

static inline vfs_filetype_t convertDirtype(uint8_t t)
{
    switch (t) {
    case DT_UNKNOWN:    return VFS_FILETYPE_UNKNOWN;
    case DT_FIFO:       return VFS_FILETYPE_REGULAR_FILE;
    case DT_CHR:        return VFS_FILETYPE_CHARACTER_DEVICE;
    case DT_DIR:        return VFS_FILETYPE_DIRECTORY;
    case DT_BLK:        return VFS_FILETYPE_BLOCK_DEVICE;
    case DT_REG:        return VFS_FILETYPE_REGULAR_FILE;
    case DT_LNK:        return VFS_FILETYPE_SYMBOLIC_LINK;
    case DT_SOCK:       return VFS_FILETYPE_SOCKET_STREAM;
    case DT_WHT:        return VFS_FILETYPE_UNKNOWN;
    default:            return VFS_FILETYPE_UNKNOWN;
    }
}

static int _Open(const char *path, int flags)
{
    int mode = 0644;
    int fd = open(path, flags, mode);
    if (fd < 0) return -errno;
    return fd;
}

static int _OpenAt(int fd, const char *path, int flags)
{
    int mode = 0644;
    int ret = openat(fd, path, flags, mode);
    if (ret < 0) return -errno;
    return ret;
}

static int _Close(int fd)
{
    int ret = close(fd);
    if (ret < 0) return -errno;
    return ret;
}

static int _FdStat(int fd, vfs_fdstat_t *s)
{
    struct stat fd_stat;

    int fl = fcntl(fd, F_GETFL);
    if (fl < 0) return -errno;

    fstat(fd, &fd_stat);
    int mode = fd_stat.st_mode;
    s->filetype = convertFiletype(mode);
    s->flags = ((fl & O_APPEND)    ? VFS_O_APPEND    : 0) |
               ((fl & O_DSYNC)     ? VFS_O_DSYNC     : 0) |
               ((fl & O_NONBLOCK)  ? VFS_O_NONBLOCK  : 0) |
               ((fl & O_RSYNC)     ? VFS_O_RSYNC     : 0) |
               ((fl & O_SYNC)      ? VFS_O_SYNC      : 0);

    return 0;
}

static inline uint64_t convertTimespec(const struct timespec *ts)
{
    if (ts->tv_sec < 0)
        return 0;
    if ((uint64_t)ts->tv_sec >= UINT64_MAX / 1000000000ULL)
        return UINT64_MAX;
    return (uint64_t)ts->tv_sec * 1000000000ULL + ts->tv_nsec;
}

static int _FileStatAt(int fd, const char *path, vfs_filestat_t *s)
{
    struct stat statbuf;

    int ret = fstatat(fd, path, &statbuf, 0);
    if (ret < 0) return -errno;

    s->filetype = convertFiletype(statbuf.st_mode);
    s->dev = *((uint32_t *)vfs_linux_drv.id); //statbuf.st_dev;
    s->ino = statbuf.st_ino;
    s->nlink = statbuf.st_nlink;
    s->size = statbuf.st_size;
    s->atim = convertTimespec(&statbuf.st_atim);
    s->mtim = convertTimespec(&statbuf.st_mtim);
    s->ctim = convertTimespec(&statbuf.st_ctim);

    return 0;
}

static int _Read(int fd, void *buf, size_t nbyte)
{
    int ret = read(fd, buf, nbyte);
    if (ret < 0) return -errno;
    return ret;
}

static int  _Write(int fd, const void *buf, size_t nbyte)
{
    int ret = write(fd, buf, nbyte);
    if (ret < 0) return -errno;
    return ret;
}

static int _Seek(int fd, long off, int whence, long *pos)
{
    if (pos == NULL) return -EINVAL;

    errno = 0;
    *pos = lseek(fd, off, whence);
    if (errno) return -errno;

    return 0;
}

static int _Tell(int fd, long *pos)
{
    if (pos == NULL) return -EINVAL;

    errno = 0;
    *pos = lseek(fd, 0, SEEK_CUR);
    if (errno) return -errno;

    return 0;
}

static int _ReadDir(int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    vfs_dirent_t dir;
    size_t used = 0;
    DIR *dp = fdopendir(fd);
    struct dirent *ep;

    if (dp != NULL)
    {
        if (*cookie != 0) {
            seekdir(dp, (long)*cookie);
        }

        while ((ep = readdir (dp)))
        {
            dir.d_ino       = ep->d_ino;
            dir.d_namlen    = strnlen(ep->d_name, 256);
            dir.d_type      = convertDirtype(ep->d_type);
            dir.d_next      = telldir(dp);

            if (used + sizeof(dir) + dir.d_namlen > bufLen) {
                used = bufLen;
                break;
            }
            memcpy(buf + used, &dir, sizeof(dir));
            memcpy(buf + sizeof(dir) + used, ep->d_name, dir.d_namlen);

            used += sizeof(dir) + dir.d_namlen;
        }
    } else {
        return -errno;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}
