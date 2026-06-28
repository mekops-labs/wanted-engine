/* SPDX-License-Identifier: Apache-2.0 */

#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <config-linux.h>
#include <cwalk.h>
#include <debug_trace.h>
#include <platform.h>
#include <vfs-posix.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

/* The Linux host has no driver the engine drives directly: gpio/wifi are
 * hardware capabilities with no host backing, so the platform offers none. A
 * launch config naming such a driver fails with -ENODEV. A real backing
 * (sysfs/libgpiod) would add an entry here. */
static const vfs_driver_table_t linux_driver_table[] = {
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) {
    return linux_driver_table;
}

static const char id[] = {'L', 'i', 'n', 'u'};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);
static int _Rename(vfs_driver_ctx_t d, int old_fd, const char *old_path,
                   int new_fd, const char *new_path);
static int _Mkdir(vfs_driver_ctx_t d, int fd, const char *path);

struct vfs_driver_ctx_t {
    const char *rootPath;
    bool readonly;
};

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *options,
                                bool readonly) {
    const char *root;
    vfs_driver_t *driver;
    (void)wapp;

    if (NULL == options) {
        root = DEFAULT_ROOT;
    } else {
        root = options;
    }

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }

    size_t rootLen = strnlen(root, PATH_MAX);
    driver->ctx->rootPath = (char *)WantedMalloc(rootLen + 1);
    if (NULL == driver->ctx->rootPath) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver->ctx);
        WantedFree(driver);
        return NULL;
    }

    char nullChar = '\0';
    if (rootLen > 0) {
        memcpy((char *)driver->ctx->rootPath, root, rootLen);
    }
    memcpy((char *)&driver->ctx->rootPath[rootLen], &nullChar, 1);
    driver->ctx->readonly = readonly;

    driver->bytesId = *(uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->OpenAt = _OpenAt;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Seek = _Seek;
    driver->ReadDir = _ReadDir;
    driver->Rename = _Rename;
    driver->Mkdir = _Mkdir;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree((void *)d->ctx->rootPath);
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static inline vfs_filetype_t convertDirtype(uint8_t t) {
    switch (t) {
    case DT_UNKNOWN:
        return VFS_FILETYPE_UNKNOWN;
    case DT_FIFO:
        return VFS_FILETYPE_REGULAR_FILE;
    case DT_CHR:
        return VFS_FILETYPE_CHARACTER_DEVICE;
    case DT_DIR:
        return VFS_FILETYPE_DIRECTORY;
    case DT_BLK:
        return VFS_FILETYPE_BLOCK_DEVICE;
    case DT_REG:
        return VFS_FILETYPE_REGULAR_FILE;
    case DT_LNK:
        return VFS_FILETYPE_SYMBOLIC_LINK;
    case DT_SOCK:
        return VFS_FILETYPE_SOCKET_STREAM;
    case DT_WHT:
        return VFS_FILETYPE_UNKNOWN;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    char joined[PATH_MAX];
    cwk_path_change_root(path, d->rootPath, joined, sizeof(joined));

    int fl = convertVfsFlags(flags);

    DEBUG_TRACE("flags: %x, path: %s", fl, joined);
    int mode = 0644;
    int fd = open(joined, fl, mode);
    if (fd < 0)
        return -errno;
    return fd;
}

/* openat() that refuses to resolve outside the preopen directory.
 * RESOLVE_BENEATH rejects absolute paths, escaping ".." and — the case the
 * read-only flag cannot close — a symlink inside the host directory that points
 * outside it. Resolution is confined to the subtree under `dirfd`.
 *
 * openat2() (Linux >= 5.6) is required, with no plain-openat() fallback: on a
 * kernel that lacks it the syscall returns ENOSYS and the open fails loudly,
 * rather than silently resolving without the escape guard. Confinement is not
 * optional — a sandbox we cannot enforce must deny, not degrade. */
static int openAtBeneath(int dirfd, const char *path, int flags, int mode) {
    struct open_how how;
    memset(&how, 0, sizeof(how));
    how.flags = (uint64_t)(unsigned int)flags;
    how.mode = (flags & O_CREAT) ? (uint64_t)(unsigned int)mode : 0;
    how.resolve = RESOLVE_BENEATH;

    return (int)syscall(SYS_openat2, dirfd, path, &how, sizeof(how));
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    if (d->readonly && VFS_O_IS_WRITE(flags))
        return -EROFS;
    /* `fd` is the preopen directory's host fd and `path` is already relative to
     * it, so the kernel resolves against `fd` directly. Prepending d->rootPath
     * would be both redundant and wrong (cwk_path_change_root drops the
     * separator, yielding e.g. "/dir" + "file" → "/dirfile"). */
    int fl = convertVfsFlags(flags);

    DEBUG_TRACE("fd: %d, flags: 0x%x, path: %s", fd, fl, path);

    int mode = 0644;
    int ret = openAtBeneath(fd, path, fl, mode);
    if (ret < 0)
        return -errno;

    return ret;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d;
    /* The console stream slots borrow the engine's native stdio (fd 0/1/2). The
     * VFS does not own those — they belong to the process and must survive a
     * supervisor teardown so the respawned/re-exec'd supervisor still has a
     * console. Files this driver opens always get fd >= 3, so this only ever
     * spares the borrowed stdio. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
        return 0;
    int ret = close(fd);
    if (ret < 0)
        return -errno;
    return ret;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    int ret, fl;
    struct stat statbuf;
    (void)d;

    fl = fcntl(fd, F_GETFL);
    if (fl < 0)
        return -errno;

    ret = fstat(fd, &statbuf);
    if (ret < 0)
        return -errno;

    s->filetype = convertFiletype(statbuf.st_mode);
    s->dev = *(uint32_t *)(id);
    s->ino = statbuf.st_ino;
    s->nlink = statbuf.st_nlink;
    s->size = statbuf.st_size;
    s->atim = convertTimespec(&statbuf.st_atim);
    s->mtim = convertTimespec(&statbuf.st_mtim);
    s->ctim = convertTimespec(&statbuf.st_ctim);
    s->oflags = ((fl & O_APPEND) ? VFS_O_APPEND : 0) |
                ((fl & O_DSYNC) ? VFS_O_DSYNC : 0) |
                ((fl & O_NONBLOCK) ? VFS_O_NONBLOCK : 0) |
                ((fl & O_RSYNC) ? VFS_O_RSYNC : 0) |
                ((fl & O_SYNC) ? VFS_O_SYNC : 0);

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)d;
    int ret = read(fd, buf, nbyte);
    if (ret < 0)
        return -errno;
    return ret;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (d->readonly)
        return -EROFS;
    int ret = write(fd, buf, nbyte);
    if (ret < 0)
        return -errno;
    return ret;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    (void)d;
    if (pos == NULL)
        return -EINVAL;

    errno = 0;
    *pos = lseek(fd, off, whence);
    if (errno)
        return -errno;

    return 0;
}

static int _Rename(vfs_driver_ctx_t d, int old_fd, const char *old_path,
                   int new_fd, const char *new_path) {
    if (d->readonly)
        return -EROFS;
    return PlatformFsRename(old_fd, old_path, new_fd, new_path);
}

static int _Mkdir(vfs_driver_ctx_t d, int fd, const char *path) {
    if (d->readonly)
        return -EROFS;
    return PlatformFsMkdir(fd, path);
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    vfs_dirent_t dir = {0};
    size_t used = 0;
    DIR *dp = fdopendir(fd);
    (void)d;
    struct dirent *ep;

    if (dp != NULL) {
        if (*cookie != 0) {
            seekdir(dp, (long)*cookie);
        }

        while ((ep = readdir(dp))) {
            if (memcmp(".", ep->d_name, 2) == 0 ||
                memcmp("..", ep->d_name, 3) == 0) {
                continue;
            }
            dir.d_ino = ep->d_ino;
            dir.d_namlen = strnlen(ep->d_name, sizeof(ep->d_name));
            dir.d_type = convertDirtype(ep->d_type);
            dir.d_next = telldir(dp);

            if (used + sizeof(dir) + dir.d_namlen > bufLen) {
                used = bufLen;
                break;
            }
            memcpy((char *)buf + used, &dir, sizeof(dir));
            memcpy((char *)buf + sizeof(dir) + used, ep->d_name, dir.d_namlen);

            used += sizeof(dir) + dir.d_namlen;
        }
    } else {
        return -errno;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}
