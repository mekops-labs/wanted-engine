/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform VFS driver over esp_vfs (LittleFS-backed). */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h> /* rename() */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <config-esp-idf.h>
#include <cwalk.h>
#include <debug_trace.h>
#include <platform.h>
#include <vfs-posix.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

/* Driver-private open-handle table size. */
#define ESP_IDF_VFS_MAX_OPEN 24

/* Synthetic fd base for directory handles. */
#define ESP_IDF_VFS_DIR_FD_BASE 0x10000

/* Console stdio fds (0/1/2). */
#define ESP_IDF_VFS_STDIO_FD_MAX 3

typedef struct {
    bool used;
    bool isDir;
    bool canRead;
    bool canWrite;
    int fd;
    char path[MAX_PATH_LEN];
} esp_vfs_open_t;

static esp_vfs_open_t g_open[ESP_IDF_VFS_MAX_OPEN];
static pthread_mutex_t g_openLock = PTHREAD_MUTEX_INITIALIZER;

/* Caller must hold g_openLock. */
static int recordOpenLocked(const char *path, bool isDir, int fdIn,
                            bool canRead, bool canWrite) {
    int slot = -1;
    for (int i = 0; i < ESP_IDF_VFS_MAX_OPEN; i++) {
        if (!g_open[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EMFILE;

    size_t plen = strnlen(path, MAX_PATH_LEN - 1);
    memcpy(g_open[slot].path, path, plen);
    g_open[slot].path[plen] = '\0';
    g_open[slot].isDir = isDir;
    g_open[slot].canRead = canRead;
    g_open[slot].canWrite = canWrite;
    g_open[slot].fd = isDir ? (ESP_IDF_VFS_DIR_FD_BASE + slot) : fdIn;
    g_open[slot].used = true;
    return g_open[slot].fd;
}

/* Caller must hold g_openLock. */
static esp_vfs_open_t *findByFdLocked(int fd) {
    for (int i = 0; i < ESP_IDF_VFS_MAX_OPEN; i++) {
        if (g_open[i].used && g_open[i].fd == fd)
            return &g_open[i];
    }
    return NULL;
}

/* Looks up `fd`'s recorded path and joins `relPath` onto it. */
static int joinFromFd(int fd, const char *relPath, char *out, size_t outLen) {
    pthread_mutex_lock(&g_openLock);
    const esp_vfs_open_t *e = findByFdLocked(fd);
    char base[MAX_PATH_LEN];
    if (e != NULL)
        memcpy(base, e->path, sizeof(base));
    pthread_mutex_unlock(&g_openLock);
    if (e == NULL)
        return -EBADF;

    size_t n = cwk_path_join(base, relPath, out, outLen);
    if (n >= outLen)
        return -ENAMETOOLONG;
    return 0;
}

/* mkdir -p: walk the path, creating each missing component. Existing
 * components are tolerated (EEXIST -> OK); any other mkdir failure aborts. */
static int mkdirP(const char *path, mode_t mode) {
    if (path == NULL || *path == '\0')
        return -EINVAL;
    char buf[MAX_PATH_LEN];
    size_t plen = strnlen(path, sizeof(buf));
    if (plen >= sizeof(buf))
        return -ENAMETOOLONG;
    memcpy(buf, path, plen + 1);
    for (size_t i = 1; i < plen; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST)
            return -errno;
        buf[i] = '/';
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

int PlatformOpenStateDir(const char *path, bool readonly) {
    if (path == NULL || *path == '\0')
        return -EINVAL;
    if (!readonly) {
        int rc = mkdirP(path, 0755);
        if (rc < 0)
            return rc;
    }

    /* esp_vfs has no O_DIRECTORY open to validate against. */
    struct stat st;
    if (stat(path, &st) < 0)
        return -errno;
    if (!S_ISDIR(st.st_mode))
        return -ENOTDIR;

    pthread_mutex_lock(&g_openLock);
    int fd = recordOpenLocked(path, true, -1, false, false);
    pthread_mutex_unlock(&g_openLock);
    return fd;
}

int PlatformFsRename(int old_fd, const char *old_path, int new_fd,
                     const char *new_path) {
    if (old_path == NULL || new_path == NULL)
        return -EINVAL;
    char oldAbs[MAX_PATH_LEN], newAbs[MAX_PATH_LEN];
    int rc = joinFromFd(old_fd, old_path, oldAbs, sizeof(oldAbs));
    if (rc < 0)
        return rc;
    rc = joinFromFd(new_fd, new_path, newAbs, sizeof(newAbs));
    if (rc < 0)
        return rc;
    if (rename(oldAbs, newAbs) < 0)
        return -errno;
    return 0;
}

int PlatformFsMkdir(int fd, const char *path) {
    if (path == NULL)
        return -EINVAL;
    char abs[MAX_PATH_LEN];
    int rc = joinFromFd(fd, path, abs, sizeof(abs));
    if (rc < 0)
        return rc;
    if (mkdir(abs, 0755) < 0)
        return -errno;
    return 0;
}

int PlatformFsRmdir(int fd, const char *path) {
    if (path == NULL)
        return -EINVAL;
    char abs[MAX_PATH_LEN];
    int rc = joinFromFd(fd, path, abs, sizeof(abs));
    if (rc < 0)
        return rc;
    if (rmdir(abs) < 0)
        return -errno;
    return 0;
}

static const char id[] = {'E', 'I', 'd', 'f'};

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
static int _Rmdir(vfs_driver_ctx_t d, int fd, const char *path);

struct vfs_driver_ctx_t {
    const char *rootPath;
    bool readonly;
};

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *options,
                                bool readonly) {
    const char *root;
    vfs_driver_t *driver;
    (void)wapp;

    if (options == NULL) {
        root = DEFAULT_ROOT;
    } else {
        root = options;
    }

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (driver->ctx == NULL) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }

    size_t rootLen = strnlen(root, MAX_PATH_LEN);
    driver->ctx->rootPath = (char *)WantedMalloc(rootLen + 1);
    if (driver->ctx->rootPath == NULL) {
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

    driver->bytesId = *(const uint32_t *)(id);
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
    driver->Rmdir = _Rmdir;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree((void *)d->ctx->rootPath);
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

/* ESP-IDF's runtime dirent only populates DT_UNKNOWN/DT_REG/DT_DIR. */
static inline vfs_filetype_t convertDirtype(uint8_t t) {
    switch (t) {
    case DT_REG:
        return VFS_FILETYPE_REGULAR_FILE;
    case DT_DIR:
        return VFS_FILETYPE_DIRECTORY;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

/* Opens `path` (already absolute, already root-joined) and records it in the
 * open-handle table. */
static int espOpen(const char *path, vfs_oflags_t flags) {
    if ((flags & VFS_O_DIRECTORY) != 0) {
        if ((flags & VFS_O_CREAT) != 0) {
            if (mkdir(path, 0755) < 0 && errno != EEXIST)
                return -errno;
        }
        struct stat st;
        if (stat(path, &st) < 0)
            return -errno;
        if (!S_ISDIR(st.st_mode))
            return -ENOTDIR;

        pthread_mutex_lock(&g_openLock);
        int fd = recordOpenLocked(path, true, -1, false, false);
        pthread_mutex_unlock(&g_openLock);
        return fd;
    }

    int fl = convertVfsFlags(flags);
    int mode = 0644;
    DEBUG_TRACE("flags: %x, path: %s", fl, path);
    int realFd = open(path, fl, mode);
    if (realFd < 0)
        return -errno;

    /* joltwallet/littlefs hard-asserts (aborts the whole device) if
     * lfs_file_read_/lfs_file_write_ is called on a handle opened without the
     * matching access bit, rather than returning EBADF like a POSIX
     * filesystem would. Track the access mode ourselves and reject a
     * mismatched Read/Write before it reaches the filesystem. */
    int accMode = flags & (VFS_O_WRONLY | VFS_O_RDWR);
    bool canRead = accMode != VFS_O_WRONLY;
    bool canWrite = accMode != 0;

    pthread_mutex_lock(&g_openLock);
    int fd = recordOpenLocked(path, false, realFd, canRead, canWrite);
    pthread_mutex_unlock(&g_openLock);
    if (fd < 0)
        close(realFd);
    return fd;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    char joined[MAX_PATH_LEN];
    cwk_path_change_root(path, d->rootPath, joined, sizeof(joined));
    return espOpen(joined, flags);
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    if (d->readonly && VFS_O_IS_WRITE(flags))
        return -EROFS;

    char joined[MAX_PATH_LEN];
    int rc = joinFromFd(fd, path, joined, sizeof(joined));
    if (rc < 0)
        return rc;

    DEBUG_TRACE("fd: %d, flags: 0x%x, path: %s", fd, flags, joined);
    return espOpen(joined, flags);
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d;
    /* The console stream slots borrow the engine's native stdio (fd 0/1/2),
     * never routed through this table. */
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)
        return 0;

    pthread_mutex_lock(&g_openLock);
    esp_vfs_open_t *e = findByFdLocked(fd);
    bool isDir = e != NULL && e->isDir;
    if (e != NULL)
        e->used = false;
    pthread_mutex_unlock(&g_openLock);
    if (e == NULL)
        return -EBADF;
    if (isDir)
        return 0;

    if (close(fd) < 0)
        return -errno;
    return 0;
}

/* joltwallet/littlefs's fstat reads the on-disk directory entry
 * (lfs_stat(path)) rather than the open file handle's live size
 * (lfs_file_size()), since esp_vfs's fd->path cache is the only link it has
 * back to the file. A size check against a handle with unflushed writes
 * therefore sees the last-synced size, not the pending one; it only reflects
 * reality once the handle has synced (a Close, or any operation that flushes
 * littlefs's write cache). */
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    (void)d;
    bool isDir = false;
    char path[MAX_PATH_LEN];

    if (fd >= ESP_IDF_VFS_STDIO_FD_MAX) {
        pthread_mutex_lock(&g_openLock);
        const esp_vfs_open_t *e = findByFdLocked(fd);
        isDir = e != NULL && e->isDir;
        if (e != NULL)
            memcpy(path, e->path, sizeof(path));
        pthread_mutex_unlock(&g_openLock);
        if (e == NULL)
            return -EBADF;
    }

    struct stat statbuf;
    int fl = 0;
    if (isDir) {
        if (stat(path, &statbuf) < 0)
            return -errno;
    } else {
        if (fstat(fd, &statbuf) < 0)
            return -errno;
        /* Not every esp_vfs-registered filesystem implements fcntl; treat a
         * failure as "no open-flag info available" rather than failing the
         * whole stat. */
        int f = fcntl(fd, F_GETFL);
        if (f >= 0)
            fl = f;
    }

    s->filetype = convertFiletype(statbuf.st_mode);
    s->dev = *(const uint32_t *)(id);
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
    if (fd >= ESP_IDF_VFS_DIR_FD_BASE)
        return -EISDIR;

    if (fd < ESP_IDF_VFS_STDIO_FD_MAX) {
        int ret = read(fd, buf, nbyte);
        return ret < 0 ? -errno : ret;
    }

    pthread_mutex_lock(&g_openLock);
    const esp_vfs_open_t *e = findByFdLocked(fd);
    bool canRead = e != NULL && e->canRead;
    pthread_mutex_unlock(&g_openLock);
    if (e == NULL)
        return -EBADF;
    if (!canRead)
        return -EBADF;

    int ret = read(fd, buf, nbyte);
    if (ret < 0)
        return -errno;
    return ret;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (d->readonly)
        return -EROFS;
    if (fd >= ESP_IDF_VFS_DIR_FD_BASE)
        return -EISDIR;

    if (fd < ESP_IDF_VFS_STDIO_FD_MAX) {
        int ret = write(fd, buf, nbyte);
        return ret < 0 ? -errno : ret;
    }

    pthread_mutex_lock(&g_openLock);
    const esp_vfs_open_t *e = findByFdLocked(fd);
    bool canWrite = e != NULL && e->canWrite;
    pthread_mutex_unlock(&g_openLock);
    if (e == NULL)
        return -EBADF;
    if (!canWrite)
        return -EBADF;

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
    if (fd >= ESP_IDF_VFS_DIR_FD_BASE)
        return -EISDIR;

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

static int _Rmdir(vfs_driver_ctx_t d, int fd, const char *path) {
    if (d->readonly)
        return -EROFS;
    return PlatformFsRmdir(fd, path);
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    (void)d;
    pthread_mutex_lock(&g_openLock);
    const esp_vfs_open_t *e = findByFdLocked(fd);
    char path[MAX_PATH_LEN];
    if (e != NULL)
        memcpy(path, e->path, sizeof(path));
    pthread_mutex_unlock(&g_openLock);
    if (e == NULL)
        return -EBADF;

    vfs_dirent_t dirent = {0};
    uint8_t *out = (uint8_t *)buf;
    size_t used = 0;

    DIR *dp = opendir(path);
    if (dp == NULL)
        return -errno;

    if (*cookie != 0)
        seekdir(dp, (long)*cookie);

    struct dirent *ep;
    while ((ep = readdir(dp)) != NULL) {
        if (memcmp(".", ep->d_name, 2) == 0 ||
            memcmp("..", ep->d_name, 3) == 0) {
            continue;
        }
        dirent.d_namlen = strnlen(ep->d_name, sizeof(ep->d_name));
        dirent.d_type = convertDirtype(ep->d_type);
        dirent.d_next = (uint64_t)telldir(dp);
        /* LittleFS's dirent carries no stable inode; the readdir stream
         * position is a stable per-entry identifier, and the VFS exposes
         * d_ino opaquely, so fall back to it when d_ino reads as zero. */
        dirent.d_ino = (ep->d_ino != 0) ? (uint64_t)ep->d_ino : dirent.d_next;

        if (used + sizeof(dirent) + dirent.d_namlen > bufLen) {
            used = bufLen;
            break;
        }
        memcpy(out + used, &dirent, sizeof(dirent));
        memcpy(out + used + sizeof(dirent), ep->d_name, dirent.d_namlen);

        used += sizeof(dirent) + dirent.d_namlen;
    }
    closedir(dp);

    *bufUsed = used;
    *cookie = dirent.d_next;

    return 0;
}
