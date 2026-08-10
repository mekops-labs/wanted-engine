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
/* The wapp-visible image-reference separator: a ref is "<name>[:<version>]".
 * The loader maps the resolved pair onto the on-disk filename, which uses
 * REGISTRY_VERSION_SEPARATOR. */
static const char VERSION_SEPARATOR = ':';

/* Longest install ref "<name>:<version>" (+NUL) the driver buffers between an
 * install open and its finalizing close. */
#define REG_REF_MAX (WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN + 1)

/* Concurrent opens the driver tracks. State is per descriptor: a wapp reading
 * the registry while another installs into it must not disturb the install,
 * and one close must not answer for another descriptor's writes. */
#define MAX_REG_FDS 8

typedef enum {
    REG_FD_FREE = 0,
    REG_FD_ROOT,  /* the mount root, enumerated with ReadDir */
    REG_FD_ENTRY, /* one image, read as a synthesized descriptor */
    REG_FD_WRITE, /* an install, named by the ref the open carried */
} reg_fd_kind_t;

typedef struct {
    reg_fd_kind_t kind;
    /* ENTRY: a copy rather than an index into the shared table, which an
     * unlink or another open reloads underneath. */
    reg_entry_t entry;
    bool readDone;
    /* WRITE: the install target, used to name the stored file at finalize,
     * and whether its first chunk has reached the platform writer. */
    bool startedWriting;
    char writeRef[REG_REF_MAX];
} reg_fd_t;

static struct vfs_driver_ctx_t {
    /* Shared: the enumeration a root descriptor walks. Reloaded on every open
     * that resolves a ref, so a walk in progress can see it change. */
    size_t nEntries;
    reg_entry_t entries[MAX_REG_ENTRIES];
    reg_fd_t fds[MAX_REG_FDS];
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

/* Load the entry table from the platform. Every lookup by ref needs it, and a
 * lookup is not always preceded by an open of the root — a caller that asks
 * whether one image is installed never enumerates the directory. */
static int refreshEntries(vfs_driver_ctx_t d) {
    int ret = PlatformRegistryRead(d->entries, MAX_REG_ENTRIES);

    if (ret < 0)
        return ret;
    d->nEntries = ret;

    return 0;
}

/* Whether `path` ("<name>[:<version>]") names `e`. The name half is compared
 * in full, or "tg-broker" answers for "tg-broker-v2"; a versionless ref
 * matches any version. */
static bool matchesRef(const reg_entry_t *e, const char *path) {
    const char *colon = strchr(path, (int)VERSION_SEPARATOR);
    size_t nameLen = colon != NULL ? (size_t)(colon - path) : strlen(path);

    if (strnlen(e->name, WAPP_MAX_NAME_LEN) != nameLen)
        return false;
    if (strncmp(path, e->name, nameLen) != 0)
        return false;
    if (colon == NULL)
        return true;

    return strncmp(colon + 1, e->version, WAPP_MAX_VERSION_LEN) == 0;
}

/* Claim a descriptor slot, zeroed but for its kind. */
static int allocFd(vfs_driver_ctx_t d, reg_fd_kind_t kind) {
    for (int i = 0; i < MAX_REG_FDS; i++) {
        if (d->fds[i].kind != REG_FD_FREE)
            continue;
        memset(&d->fds[i], 0, sizeof(d->fds[i]));
        d->fds[i].kind = kind;
        return i;
    }
    return -EMFILE;
}

static reg_fd_t *getFd(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= MAX_REG_FDS || d->fds[fd].kind == REG_FD_FREE)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    int ret;

    if (path == NULL)
        return -EINVAL;

    if (path[0] == '/' && path[1] == '\0') {
        ret = refreshEntries(d);
        if (ret < 0)
            return ret;
        return allocFd(d, REG_FD_ROOT);
    }

    if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) {
        /* Install by ref: opening a "<name>:<ver>" path for write names the
         * image. The ref travels to the platform writer, which names the
         * stored file by it. Image bytes go to this descriptor. */
        if (path[0] == '\0' || strlen(path) >= REG_REF_MAX)
            return -ENAMETOOLONG;
        if (!validInstallRef(path))
            return -EINVAL;
        ret = allocFd(d, REG_FD_WRITE);
        if (ret < 0)
            return ret;
        strncpy(d->fds[ret].writeRef, path, REG_REF_MAX - 1);
        d->fds[ret].writeRef[REG_REF_MAX - 1] = '\0';
        return ret;
    }

    ret = refreshEntries(d);
    if (ret < 0)
        return ret;
    for (size_t i = 0; i < d->nEntries; i++) {
        if (!matchesRef(&d->entries[i], path))
            continue;
        ret = allocFd(d, REG_FD_ENTRY);
        if (ret < 0)
            return ret;
        d->fds[ret].entry = d->entries[i];
        return ret;
    }

    return -ENOENT;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    reg_fd_t *f = getFd(d, fd);
    bool finalize;

    if (f == NULL)
        return -EBADF;

    finalize = f->kind == REG_FD_WRITE && f->startedWriting;
    memset(f, 0, sizeof(*f)); /* frees the slot: kind becomes REG_FD_FREE */

    return finalize ? WantedCloseRegistry() : 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    const reg_fd_t *f = getFd(d, fd);

    if (stat == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;

    stat->dev = WantedRegistryDriver.bytesId;
    stat->ino = fd;
    stat->filetype = f->kind == REG_FD_ROOT ? WantedRegistryDriver.filetype
                                            : VFS_FILETYPE_REGULAR_FILE;
    stat->nlink = 0;
    /* An install reports nothing: its size is known only once it finishes. */
    if (f->kind == REG_FD_ROOT)
        stat->size = d->nEntries;
    else
        stat->size = f->kind == REG_FD_ENTRY ? f->entry.size : 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    reg_fd_t *f = getFd(d, fd);
    int read;

    if (buf == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;
    if (f->kind == REG_FD_ROOT)
        return -EISDIR; /* enumerate the root with ReadDir */
    if (f->kind != REG_FD_ENTRY)
        return -EBADF; /* an install is not open for reading */
    if (f->readDone)
        return 0;

    read = WantedRenderRegistryDescriptor(&f->entry, buf, nbyte);
    if (read > 0)
        f->readDone = true;

    return read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    reg_fd_t *f = getFd(d, fd);

    if (buf == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;
    /* Only an install accepts bytes; the root and a stored image do not. */
    if (f->kind != REG_FD_WRITE)
        return -EROFS;

    return WantedWriteRegistry(&f->startedWriting, f->writeRef, buf, nbyte);
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const reg_fd_t *f = getFd(d, fd);
    vfs_dirent_t dir = {0};
    size_t used = 0;

    if (buf == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;
    if (f->kind != REG_FD_ROOT)
        return -ENOTDIR;

    /* The entry the next call starts from. It advances only over an entry
     * that reached the buffer, so one that did not fit is served next. */
    size_t next = (size_t)*cookie;

    for (size_t i = next; i < d->nEntries; i++) {
        size_t nameLen = strnlen(d->entries[i].name, WAPP_MAX_NAME_LEN);
        size_t verLen = strnlen(d->entries[i].version, WAPP_MAX_VERSION_LEN);

        dir.d_ino = i;
        dir.d_namlen = nameLen + 1 + verLen;
        dir.d_type = VFS_FILETYPE_REGULAR_FILE;
        dir.d_next = i + 1;

        /* Report the bytes written. Claiming the whole buffer hands the
         * reader the part of it this never wrote, and a reader that parses
         * that as an entry follows its length and its cookie into nothing. */
        if (used + sizeof(dir) + dir.d_namlen > bufLen)
            break;

        memcpy((char *)buf + used, &dir, sizeof(dir));
        used += sizeof(dir);
        memcpy((char *)buf + used, d->entries[i].name, nameLen);
        used += nameLen;
        memcpy((char *)buf + used, &VERSION_SEPARATOR, 1);
        used += 1;
        memcpy((char *)buf + used, d->entries[i].version, verLen);
        used += verLen;
        next = i + 1;
    }

    *bufUsed = used;
    *cookie = next;

    return 0;
}

static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path) {
    int ret;
    (void)fd;

    if (path == NULL)
        return -EINVAL;

    ret = refreshEntries(d);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < d->nEntries; i++) {
        if (matchesRef(&d->entries[i], path))
            return WantedRegistryRemove(&d->entries[i]);
    }

    return -ENOENT;
}
