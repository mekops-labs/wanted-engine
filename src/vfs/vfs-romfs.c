#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <romfs.h>
#include <vfs.h>

static int  _Open(const char *path, int flags);
static int  _OpenAt(int fd, const char *path, int flags);
static int  _Close(int fd);
static int  _FdStat(int fd, vfs_filestat_t *stat);
static int  _FdStatAt(int fd, const char *path, vfs_filestat_t *stat);
static int  _Read(int fd, void *buf, size_t nbyte);
static long _Seek(int fd, long off, int whence);
static int  _ReadDir(int fd, void *buf, size_t bufLen, uint32_t *cookie, size_t *bufUsed);

vfs_driver_t vfs_romfs_drv = {
    .id = { 'R', 'o', 'm', 'f' },
    .OpenAt      = _OpenAt,
    .Close       = _Close,
    .FdStat      = _FdStat,
    .FdStatAt    = _FdStatAt,
    .Read        = _Read,
    .Write       = NULL,     // not implemented
    .Seek        = _Seek,
    .Tell        = NULL,     // not implemented
    .ReadDir     = _ReadDir,
};

static vfs_filetype_t convertFiletype(uint8_t t)
{
    switch (t & ROMFS_TYPE_MASK) {
    case ROMFS_TYPE_HARDLINK:   return VFS_FILETYPE_SYMBOLIC_LINK;
    case ROMFS_TYPE_DIRECTORY:  return VFS_FILETYPE_DIRECTORY;
    case ROMFS_TYPE_FILE:       return VFS_FILETYPE_REGULAR_FILE;
    case ROMFS_TYPE_SOFTLINK:   return VFS_FILETYPE_SYMBOLIC_LINK;
    case ROMFS_TYPE_BLOCKDEV:   return VFS_FILETYPE_BLOCK_DEVICE;
    case ROMFS_TYPE_CHARDEV:    return VFS_FILETYPE_CHARACTER_DEVICE;
    case ROMFS_TYPE_SOCKET:     return VFS_FILETYPE_SOCKET_STREAM;
    case ROMFS_TYPE_FIFO:       return VFS_FILETYPE_REGULAR_FILE;
    default:                    return VFS_FILETYPE_UNKNOWN;
    }
}

static int _Open(const char *path, int flags)
{
    return RomfsOpenRoot(path, flags);
}

static int _OpenAt(int fd, const char *path, int flags)
{
    return RomfsOpenAt(fd, path, flags);
}

static int _Close(int fd)
{
    return RomfsClose(fd);
}

static int _FdStat(int fd, vfs_filestat_t *stat)
{
    int ret;
    romfs_stat_t romfsStat;

    if (NULL == stat) return -EINVAL;

    ret = RomfsFdStat(fd, &romfsStat);
    if (ret < 0) return ret;

    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->dev = *((uint32_t *)vfs_romfs_drv.id);
    stat->ino = romfsStat.ino;
    stat->size = romfsStat.size;
    stat->filetype = convertFiletype(romfsStat.mode);

    return 0;
}

static int _FdStatAt(int fd, const char *path, vfs_filestat_t *stat)
{
    int ret;
    romfs_stat_t romfsStat;

    if (NULL == stat) return -EINVAL;

    ret = RomfsFdStatAt(fd, path, &romfsStat);
    if (ret < 0) return ret;

    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->dev = *((uint32_t *)vfs_romfs_drv.id);
    stat->ino = romfsStat.ino;
    stat->size = romfsStat.size;
    stat->filetype = convertFiletype(romfsStat.mode);

    return 0;
}

static int _Read(int fd, void *buf, size_t nbyte)
{
    return RomfsRead(fd, buf, nbyte);
}

static long _Seek(int fd, long off, int whence)
{
    return RomfsSeek(fd, off, (romfs_seek_t)whence);
}

#define DIR_BUF_LEN 10

static int _ReadDir(int fd, void *buf, size_t bufLen, uint32_t *cookie, size_t *bufUsed)
{
    int ret;
    romfs_dirent_t dir[DIR_BUF_LEN];
    vfs_dirent_t entry;
    uint32_t last = *cookie;
    size_t used;
    size_t bUsed = 0;

    if (NULL == buf || NULL == cookie || NULL == bufUsed) return -EINVAL;

    do {
        ret = RomfsReadDir(fd, dir, DIR_BUF_LEN, cookie, &used);
        if (ret < 0) return ret;

        for (size_t i = 0; i < used; i++) {
            entry.d_next = dir[i].next;
            entry.d_ino = dir[i].inode;
            entry.d_namlen = dir[i].nameLen;
            entry.d_type = convertFiletype(dir[i].type);

            if (bUsed + sizeof(entry) + entry.d_namlen > bufLen) {
                bUsed = bufLen;
                break;
            }

            memcpy(buf + bUsed, &entry, sizeof(entry));
            bUsed += sizeof(entry);
            memcpy(buf + bUsed, dir[i].name, entry.d_namlen);
            bUsed += entry.d_namlen;
        }

        if (*cookie == ROMFS_COOKIE_LAST || bufLen == bUsed) break;
    } while (used == DIR_BUF_LEN);

    *bufUsed = bUsed;

    return ret;
}
