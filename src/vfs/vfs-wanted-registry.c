/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <stdio.h>

#include <vfs-drivers.h>
#include <vfs.h>

#include <wanted-vfs-api.h>

#include <platform.h>

#define ID {'W', 'r', 'e', 'g'}

/* Registry capacity: the most image entries a single mount tracks. */
#define MAX_REG_ENTRIES 50
/* '@' (not ':') so an install ref maps to a VFAT-legal on-disk filename — see
 * REGISTRY_VERSION_SEPARATOR in platform-config.h. */
static const char VERSION_SEPARATOR = '@';

/* Longest install ref "<name>:<version>" (+NUL) the driver buffers between an
 * install open and its finalizing close. */
#define REG_REF_MAX (WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN + 1)

static struct vfs_driver_ctx_t {
    bool opened;
    bool startedWriting;
    size_t nEntries;
    reg_entry_t entries[MAX_REG_ENTRIES];
    /* Target ref for an in-progress install (opened by ref for write); empty
     * when the open is a read. Named by the open path, used to name the stored
     * file at finalize. */
    char writeRef[REG_REF_MAX];
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

/* One image-reference component (name or tag): non-empty, within `maxlen`
 * (incl. NUL), first char [A-Za-z0-9_], rest [A-Za-z0-9._-] — the OCI tag
 * grammar, applied to both halves of "<name>:<tag>". */
static bool validRefComponent(const char *s, size_t len, size_t maxlen) {
    if (len == 0 || len >= maxlen)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                     (c >= '0' && c <= '9') || c == '_';
        if (i == 0) {
            if (!alnum)
                return false;
        } else if (!alnum && c != '.' && c != '-') {
            return false;
        }
    }
    return true;
}

/* An install ref is "<name>" or "<name>:<tag>" with at most one separator; both
 * components must satisfy the tag grammar and their length bounds. This rejects
 * an out-of-grammar ref at install rather than letting it name a file. */
static bool validInstallRef(const char *ref) {
    const char *colon = strchr(ref, (int)VERSION_SEPARATOR);
    if (colon == NULL)
        return validRefComponent(ref, strlen(ref), WAPP_MAX_NAME_LEN);
    if (strchr(colon + 1, (int)VERSION_SEPARATOR) != NULL)
        return false; /* a tag carries no separator */
    return validRefComponent(ref, (size_t)(colon - ref), WAPP_MAX_NAME_LEN) &&
           validRefComponent(colon + 1, strlen(colon + 1),
                             WAPP_MAX_VERSION_LEN);
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    if (path == NULL)
        return -EINVAL;

    // if (opened) return -EBUSY;
    d->opened = true;
    d->startedWriting = false;
    d->writeRef[0] = '\0';

    if (path[0] == '/' && path[1] == '\0') {
        int ret = PlatformRegistryRead(d->entries, MAX_REG_ENTRIES);
        if (ret < 0)
            return ret;
        d->nEntries = ret;
    } else if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) {
        /* Install by ref: opening a (possibly not-yet-existing) "<name>:<ver>"
         * path for write names the image. The ref travels to the platform
         * writer, which names the stored file by it — the ref is the image's
         * identity. The image bytes are written to the root write fd (0). */
        if (path[0] == '\0' || strlen(path) >= REG_REF_MAX)
            return -ENAMETOOLONG;
        if (!validInstallRef(path))
            return -EINVAL;
        strncpy(d->writeRef, path, REG_REF_MAX - 1);
        d->writeRef[REG_REF_MAX - 1] = '\0';
        return 0;
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
    (void)fd;
    d->opened = false;

    if (d->startedWriting) {
        d->startedWriting = false;
        d->writeRef[0] = '\0';
        return WantedCloseRegistry();
    }

    d->writeRef[0] = '\0';
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

    /* fd 0 is the registry root directory; enumerate it with ReadDir. */
    if (fd == 0)
        return -EISDIR;

    static int read = 0;
    if (read > 0) {
        read = 0;
        return read;
    }

    read = WantedRenderRegistryDescriptor(&d->entries[fd - 1], buf, nbyte);

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

    /* Writes go to the root write fd, but only for an install opened by ref;
     * the registry root itself is not writable. */
    if (d->writeRef[0] == '\0')
        return -EROFS;

    ret = WantedWriteRegistry(&d->startedWriting, d->writeRef, buf, nbyte);

    return ret;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    vfs_dirent_t dir = {0};
    size_t used = 0;
    (void)fd;

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
        memcpy((char *)buf + used, &dir, sizeof(dir));
        used += sizeof(dir);
        memcpy((char *)buf + used, d->entries[i].name, nameLen);
        used += nameLen;
        memcpy((char *)buf + used, &VERSION_SEPARATOR, 1);
        used += 1;
        memcpy((char *)buf + used, d->entries[i].version, verLen);
        used += verLen;
    }

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}

static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path) {
    int i;
    (void)fd;

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
