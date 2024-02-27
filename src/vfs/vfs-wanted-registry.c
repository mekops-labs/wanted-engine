#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <stdio.h>

#include <vfs-drivers.h>
#include <vfs.h>

#include <wanted-vfs-api.h>

#include <platform.h>

#define ID                                                                     \
    { 'W', 'r', 'e', 'g' }

// TODO: make configurable
#define MAX_REG_ENTRIES 50
static const char VERSION_SEPARATOR = ':';

static struct vfs_driver_ctx_t {
    bool opened;
    bool startedWriting;
    size_t nEntries;
    reg_entry_t entries[MAX_REG_ENTRIES];
} ctx;

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);
static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path);

const vfs_driver_t WantedRegistryDriver = {
    .id = ID,
    .filetype = VFS_FILETYPE_DIRECTORY,
    .ctx = &ctx,
    .Destroy = _Destroy,
    .Open = _Open,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
    .Write = _Write,
    .ReadDir = _ReadDir,
    .Unlink = _Unlink,
};

static int _Destroy(struct vfs_driver_t *d) {
    memset(d->ctx, 0, sizeof(ctx));

    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    int ret;

    if (path == NULL)
        return -EINVAL;

    // if (opened) return -EBUSY;
    d->opened = true;
    d->startedWriting = false;

    if (path[0] == '/' && path[1] == '\0') {
        ret = PlatformRegistryRead(d->entries, MAX_REG_ENTRIES);
        if (ret < 0)
            return ret;
        d->nEntries = ret;
    } else {
        for (int i = 0; i < d->nEntries; i++) {
            const char *ver = strchr(path, (int)VERSION_SEPARATOR);
            if (ver != NULL) {
                ver += 1;
                if (strncmp(path, d->entries[i].name,
                            strnlen(d->entries[i].name, WAPP_MAX_NAME_LEN)) ==
                    0) {
                    if (ver == NULL || strncmp(ver, d->entries[i].version,
                                               WAPP_MAX_VERSION_LEN) == 0) {
                        return i + 1;
                    }
                }
            } else {
                if (strncmp(path, d->entries[i].name, WAPP_MAX_NAME_LEN) == 0) {
                    return i + 1;
                }
            }
        }
        return -ENOENT;
    }

    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    d->opened = false;

    if (d->startedWriting) {
        d->startedWriting = false;
        return WantedCloseRegistry();
    }

    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    if (fd < 0 || fd > d->nEntries)
        return -EBADF;

    stat->dev = WantedRegistryDriver.bytesId;
    stat->ino = fd;
    stat->filetype =
        fd > 0 ? VFS_FILETYPE_REGULAR_FILE : WantedRegistryDriver.filetype;
    stat->nlink = 0;
    stat->size = fd == 0 ? d->nEntries : d->entries[fd - 1].size;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (fd > d->nEntries)
        return -EINVAL;

    if (!d->opened)
        return -EBADF;

    static int read = 0;
    if (read > 0) {
        read = 0;
        return read;
    }

    if (fd == 0) {
        read = WantedReadRegistry(buf, nbyte);
    } else {
        read = WantedReadManifest(&d->entries[fd - 1], buf, nbyte);
    }

    return read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    int ret;

    if (buf == NULL)
        return -EINVAL;
    if (!d->opened)
        return -EBADF;
    if (fd > 0)
        return -EROFS;

    ret = WantedWriteRegistry(&d->startedWriting, buf, nbyte);

    return ret;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    vfs_dirent_t dir;
    size_t used = 0;

    if (buf == NULL)
        return -EINVAL;
    if (!d->opened)
        return -EBADF;

    for (int i = *cookie; i < d->nEntries; i++) {
        size_t nameLen = strnlen(d->entries[i].name, WAPP_MAX_NAME_LEN);
        size_t verLen = strnlen(d->entries[i].version, WAPP_MAX_VERSION_LEN);

        dir.d_ino = i;
        dir.d_namlen = nameLen + 1 + verLen;
        dir.d_type = VFS_FILETYPE_REGULAR_FILE;
        dir.d_next = i + 1;

        if (used + sizeof(dir) + dir.d_namlen > bufLen) {
            used = bufLen;
            break;
        }
        memcpy(buf + used, &dir, sizeof(dir));
        used += sizeof(dir);
        memcpy(buf + used, d->entries[i].name, nameLen);
        used += nameLen;
        memcpy(buf + used, &VERSION_SEPARATOR, 1);
        used += 1;
        memcpy(buf + used, d->entries[i].version, verLen);
        used += verLen;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}

static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path) {
    int i;

    for (i = 0; i < d->nEntries; i++) {
        const char *ver = strchr(path, (int)VERSION_SEPARATOR);
        if (ver != NULL) {
            ver += 1;
            if (strncmp(path, d->entries[i].name,
                        strnlen(d->entries[i].name, WAPP_MAX_NAME_LEN)) == 0) {
                if (ver == NULL || strncmp(ver, d->entries[i].version,
                                           WAPP_MAX_VERSION_LEN) == 0) {
                    break;
                }
            }
        } else {
            if (strncmp(path, d->entries[i].name, WAPP_MAX_NAME_LEN) == 0) {
                break;
            }
        }
    }

    if (i < d->nEntries) {
        return WantedRegistryRemove(&d->entries[i]);
    }

    return -ENOENT;
}
