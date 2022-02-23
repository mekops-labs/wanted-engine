#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <wanted_malloc.h>
#include <romfs.h>
#include <vfs.h>
#include <vfs-drivers.h>
#include <cwalk.h>

static const char id[] = { 'R', 'o', 'm', 'f' };

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    return -EROFS;
}

struct vfs_driver_ctx_t {
    const char* rootPath;
    romfs_t     romfs;
};

int VfsRomfsInit(vfs_driver_t *driver, const char *root, uint8_t *RomfsImg, size_t RomfsImgLen)
{
    int ret;

    if (NULL == root || NULL == RomfsImg || NULL == driver) {
        return -EINVAL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) return -ENOMEM;

    ret = RomfsLoad(RomfsImg, RomfsImgLen, &(driver->ctx->romfs));
    if (ret < 0) { WantedFree(driver->ctx); return ret; }

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = VFS_FILETYPE_DIRECTORY;
    driver->ctx->rootPath   = root;
    driver->Open            = _Open;
    driver->OpenAt          = _OpenAt;
    driver->Close           = _Close;
    driver->Stat            = _Stat;
    driver->Read            = _Read;
    driver->Write           = _Write;
    driver->Seek            = _Seek;
    driver->ReadDir         = _ReadDir;

    return 0;
}

void VfsRomfsDestroy(vfs_driver_t *driver)
{
    RomfsUnload(&(driver->ctx->romfs));
    WantedFree(driver->ctx);
}

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

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    char normalized[MAX_PATH_LEN];

    cwk_path_change_root(path, d->rootPath, normalized, sizeof(normalized));
    cwk_path_normalize(normalized, normalized, sizeof(normalized));

    return RomfsOpenRoot(d->romfs, normalized, flags);
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    char normalized[MAX_PATH_LEN];

    cwk_path_change_root(path, d->rootPath, normalized, sizeof(normalized));
    cwk_path_normalize(normalized, normalized, sizeof(normalized));
    return RomfsOpenAt(d->romfs, fd, normalized, flags);
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    return RomfsClose(d->romfs, fd);
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    int ret;
    romfs_stat_t romfsStat;

    if (NULL == stat) return -EINVAL;

    ret = RomfsFdStat(d->romfs, fd, &romfsStat);
    if (ret < 0) return ret;

    stat->dev = *(uint32_t *)id;
    stat->ino = romfsStat.ino;
    stat->filetype = convertFiletype(romfsStat.mode);
    stat->size = romfsStat.size;
    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->oflags = 0;

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    return RomfsRead(d->romfs, fd, buf, nbyte);
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos)
{
    int ret;

    if (whence != VFS_SEEK_CUR || off != 0) {
        ret = RomfsSeek(d->romfs, fd, off, (romfs_seek_t)whence);
        if (ret < 0) { return ret; }
    }

    ret = RomfsTell(d->romfs, fd, pos);

    return ret;
}

#define DIR_BUF_LEN 10

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    int ret;
    romfs_dirent_t dir[DIR_BUF_LEN];
    vfs_dirent_t entry;
    uint32_t last = *cookie;
    size_t used;
    size_t bUsed = 0;

    if (NULL == buf || NULL == cookie || NULL == bufUsed) return -EINVAL;

    do {
        ret = RomfsReadDir(d->romfs, fd, dir, DIR_BUF_LEN, &last, &used);
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

        if (last == ROMFS_COOKIE_LAST || bufLen == bUsed) break;
    } while (used == DIR_BUF_LEN);

    *bufUsed = bUsed;
    *cookie = last;

    return ret;
}
