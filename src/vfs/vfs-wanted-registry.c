/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <stdio.h>

#include <vfs-drivers.h>
#include <vfs-tarfs.h>
#include <vfs.h>

#include <wanted-vfs-api.h>
#include <wanted_log.h>

#include <platform.h>
#include <registry.h>

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
    REG_FD_ROOT,     /* the mount root: blobs, capacity, images */
    REG_FD_IMAGES,   /* images/, enumerated with ReadDir */
    REG_FD_BLOBS,    /* blobs/, enumerated with ReadDir */
    REG_FD_BLOB,     /* one held blob, read as a synthesized descriptor */
    REG_FD_CAPACITY, /* capacity, the stores as one JSON array */
    REG_FD_ENTRY,    /* one image, read as a synthesized descriptor */
    REG_FD_WRITE,    /* an install, named by the ref the open carried */
    REG_FD_SIG,      /* a signature for the ref the open carried */
    REG_FD_LAYER,    /* one layer of an image, named by ref and index */
    REG_FD_COMMIT,   /* the commit of an image's staged layers */
    REG_FD_LINK,     /* a held blob staged as one layer of an image */
} reg_fd_kind_t;

/* The children of the mount root. */
#define REG_DIR_IMAGES "images"
#define REG_DIR_BLOBS "blobs"
#define REG_NODE_CAPACITY "capacity"
/* Sibling route carrying an installed image's signature: "<ref>.sig". */
#define REG_SIG_SUFFIX ".sig"
/* Sibling route staging a held blob as a layer: "<ref>#<n>.link". */
#define REG_LINK_SUFFIX ".link"
#define REG_LINK_MAX (REGISTRY_META_DIGEST_HEX_LEN + 1)
#define REG_CAPACITY_MAX 640
#define REG_BLOB_BODY_MAX 128
_Static_assert(REG_LINK_MAX <= REGISTRY_SIG_PAYLOAD_LEN,
               "a link payload shares the signature buffer");
/* Routes for an image of several layers: "<ref>#<n>" stages layer n, counted
 * from the base, and "<ref>#layers" commits the layers staged for the ref. */
#define REG_ROUTE_SEP '#'
#define REG_ROUTE_COMMIT "layers"
#define REG_LAYER_DIGITS 2
/* Commit payload: the layer count, then optionally the image build time as a
 * big-endian int64. */
#define REG_COMMIT_MAX 9

typedef struct {
    reg_fd_kind_t kind;
    /* ENTRY: holds its own copy, since an unlink or another open
     * reloads the shared table underneath an open descriptor. */
    reg_entry_t entry;
    bool readDone;
    /* WRITE: the install target, used to name the stored file at finalize,
     * and whether its first chunk has reached the platform writer. */
    bool startedWriting;
    char writeRef[REG_REF_MAX];
    /* SIG and LINK: the payload buffered until close, which applies it whole.
     * A link payload is a digest in hex and an optional newline. */
    uint8_t sig[REGISTRY_SIG_PAYLOAD_LEN];
    size_t sigLen;
    /* BLOB: the descriptor, rendered at open. CAPACITY and BLOB: bytes read. */
    char blob[REG_BLOB_BODY_MAX];
    size_t blobLen;
    size_t readOff;
    /* LAYER: the layer index. COMMIT: the payload buffered until close. */
    unsigned layer;
    uint8_t commit[REG_COMMIT_MAX];
    size_t commitLen;
} reg_fd_t;

static struct vfs_driver_ctx_t {
    /* Shared: the enumeration a root descriptor walks. Reloaded on every open
     * that resolves a ref, so a walk in progress can see it change. */
    size_t nEntries;
    reg_entry_t entries[MAX_REG_ENTRIES];
    reg_fd_t fds[MAX_REG_FDS];
    /* The capacity body, rendered at open and served to the one reader. */
    bool capacityOpen;
    char capacity[REG_CAPACITY_MAX];
    size_t capacityLen;
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
    int ret = RegistryRead(d->entries, MAX_REG_ENTRIES);

    if (ret < 0)
        return ret;
    /* The registry total, capped at what the call filled. */
    d->nEntries = ret > MAX_REG_ENTRIES ? MAX_REG_ENTRIES : ret;

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

/* Opens "<ref>#<n>", "<ref>#<n>.link" or "<ref>#layers" for write. */
static int openLayerRoute(vfs_driver_ctx_t d, const char *path) {
    const char *sep = strchr(path, REG_ROUTE_SEP);
    const char *route = sep + 1;
    size_t refLen = (size_t)(sep - path);
    char ref[REG_REF_MAX];
    unsigned layer = 0;
    size_t digits = strlen(route);
    size_t linkLen = sizeof(REG_LINK_SUFFIX) - 1;
    bool link = digits > linkLen &&
                strcmp(route + digits - linkLen, REG_LINK_SUFFIX) == 0;
    int fd;

    if (link)
        digits -= linkLen;

    if (refLen == 0 || refLen >= REG_REF_MAX)
        return -ENAMETOOLONG;
    memcpy(ref, path, refLen);
    ref[refLen] = '\0';
    if (!validInstallRef(ref))
        return -EINVAL;

    if (strcmp(route, REG_ROUTE_COMMIT) == 0) {
        fd = allocFd(d, REG_FD_COMMIT);
    } else {
        if (digits == 0 || digits > REG_LAYER_DIGITS)
            return -EINVAL;
        for (size_t i = 0; i < digits; i++) {
            if (route[i] < '0' || route[i] > '9')
                return -EINVAL;
            layer = layer * 10 + (unsigned)(route[i] - '0');
        }
        if (layer >= TARFS_MAX_LAYERS) {
            LOG_ERROR("%s: layer index %u is at the limit of %u layers", ref,
                      layer, (unsigned)TARFS_MAX_LAYERS);
            return -EINVAL;
        }
        fd = allocFd(d, link ? REG_FD_LINK : REG_FD_LAYER);
    }
    if (fd < 0)
        return fd;
    memcpy(d->fds[fd].writeRef, ref, refLen + 1);
    d->fds[fd].layer = layer;
    return fd;
}

/* Opens "images/<rest>": the directory itself, one image to read, or an install
 * route to write. */
static int openImage(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    int ret;

    if (path[0] == '\0') {
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR))
            return -EISDIR;
        ret = refreshEntries(d);
        if (ret < 0)
            return ret;
        return allocFd(d, REG_FD_IMAGES);
    }

    if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) {
        /* Install by ref: opening a "<name>:<ver>" path for write names the
         * image. The ref travels to the platform writer, which names the
         * stored file by it. Image bytes go to this descriptor. */
        size_t len = strlen(path);
        size_t suffixLen = sizeof(REG_SIG_SUFFIX) - 1;

        if (strchr(path, REG_ROUTE_SEP) != NULL)
            return openLayerRoute(d, path);

        if (len > suffixLen &&
            strcmp(path + len - suffixLen, REG_SIG_SUFFIX) == 0) {
            char ref[REG_REF_MAX];
            size_t refLen = len - suffixLen;
            if (refLen >= REG_REF_MAX)
                return -ENAMETOOLONG;
            memcpy(ref, path, refLen);
            ref[refLen] = '\0';
            if (!validInstallRef(ref))
                return -EINVAL;
            ret = allocFd(d, REG_FD_SIG);
            if (ret < 0)
                return ret;
            memcpy(d->fds[ret].writeRef, ref, refLen + 1);
            return ret;
        }

        if (len >= REG_REF_MAX)
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

/* Opens "blobs/<rest>": the directory, or one blob named by its digest. */
static int openBlob(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    char body[REG_BLOB_BODY_MAX];
    int ret;
    int n;

    if (flags & (VFS_O_WRONLY | VFS_O_RDWR))
        return -EROFS;
    if (path[0] == '\0')
        return allocFd(d, REG_FD_BLOBS);
    ret = RegistryDigestFromHex(path, strlen(path), digest);
    if (ret < 0)
        return ret;
    ret = WantedRenderRegistryBlob(digest, body, sizeof(body));
    if (ret < 0)
        return ret;
    n = ret;
    ret = allocFd(d, REG_FD_BLOB);
    if (ret < 0)
        return ret;
    memcpy(d->fds[ret].blob, body, (size_t)n);
    d->fds[ret].blobLen = (size_t)n;
    return ret;
}

static int openCapacity(vfs_driver_ctx_t d, vfs_oflags_t flags) {
    int n;
    int ret;

    if (flags & (VFS_O_WRONLY | VFS_O_RDWR))
        return -EROFS;
    if (d->capacityOpen)
        return -EBUSY;
    n = WantedRenderRegistryCapacity(d->capacity, sizeof(d->capacity));
    if (n < 0)
        return n;
    ret = allocFd(d, REG_FD_CAPACITY);
    if (ret < 0)
        return ret;
    d->capacityLen = (size_t)n;
    d->capacityOpen = true;
    return ret;
}

/* Whether the next segment of `path` is `name`, ending at a '/' or the end. On
 * a match `*rest` is what follows, without its leading '/'. */
static bool takeSegment(const char *path, const char *name, const char **rest) {
    size_t n = strlen(name);

    if (strncmp(path, name, n) != 0 || (path[n] != '\0' && path[n] != '/'))
        return false;
    *rest = path[n] == '/' ? path + n + 1 : path + n;
    return true;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    const char *rest;

    if (path == NULL)
        return -EINVAL;
    while (path[0] == '/')
        path++;
    if (path[0] == '\0') {
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR))
            return -EISDIR;
        return allocFd(d, REG_FD_ROOT);
    }
    if (takeSegment(path, REG_DIR_IMAGES, &rest))
        return openImage(d, rest, flags);
    if (takeSegment(path, REG_DIR_BLOBS, &rest))
        return openBlob(d, rest, flags);
    if (takeSegment(path, REG_NODE_CAPACITY, &rest) && rest[0] == '\0')
        return openCapacity(d, flags);

    return -ENOENT;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    reg_fd_t *f = getFd(d, fd);
    bool finalize;

    if (f == NULL)
        return -EBADF;

    if (f->kind == REG_FD_SIG) {
        char ref[REG_REF_MAX];
        uint8_t payload[REGISTRY_SIG_PAYLOAD_LEN];
        size_t len = f->sigLen;

        memcpy(ref, f->writeRef, sizeof(ref));
        memcpy(payload, f->sig, sizeof(payload));
        memset(f, 0, sizeof(*f));
        /* A short payload names no signature, and applying part of one would
         * record bytes no verifier can use. */
        if (len == 0)
            return 0;
        return WantedRegistrySetSignature(ref, payload, len);
    }

    if (f->kind == REG_FD_LINK) {
        char ref[REG_REF_MAX];
        uint8_t payload[REG_LINK_MAX];
        unsigned layer = f->layer;
        size_t len = f->sigLen;

        memcpy(ref, f->writeRef, sizeof(ref));
        memcpy(payload, f->sig, sizeof(payload));
        memset(f, 0, sizeof(*f));
        if (len == 0)
            return 0;
        return WantedLinkRegistryLayer(ref, layer, payload, len);
    }

    if (f->kind == REG_FD_CAPACITY)
        d->capacityOpen = false;

    if (f->kind == REG_FD_COMMIT) {
        char ref[REG_REF_MAX];
        uint8_t payload[REG_COMMIT_MAX];
        size_t len = f->commitLen;

        memcpy(ref, f->writeRef, sizeof(ref));
        memcpy(payload, f->commit, sizeof(payload));
        memset(f, 0, sizeof(*f));
        if (len == 0)
            return 0;
        return WantedCommitRegistry(ref, payload, len);
    }

    if (f->kind == REG_FD_LAYER) {
        finalize = f->startedWriting;
        memset(f, 0, sizeof(*f));
        return finalize ? WantedCloseRegistryLayer() : 0;
    }

    finalize = f->kind == REG_FD_WRITE && f->startedWriting;
    memset(f, 0, sizeof(*f)); /* frees the slot: kind becomes REG_FD_FREE */

    return finalize ? WantedCloseRegistry() : 0;
}

static bool isDir(const reg_fd_t *f) {
    return f->kind == REG_FD_ROOT || f->kind == REG_FD_IMAGES ||
           f->kind == REG_FD_BLOBS;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    const reg_fd_t *f = getFd(d, fd);

    if (stat == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;

    stat->dev = WantedRegistryDriver.bytesId;
    stat->ino = fd;
    stat->filetype =
        isDir(f) ? WantedRegistryDriver.filetype : VFS_FILETYPE_REGULAR_FILE;
    stat->nlink = 0;
    /* An install reports nothing: its size is known only once it finishes. */
    if (f->kind == REG_FD_IMAGES)
        stat->size = d->nEntries;
    else if (f->kind == REG_FD_CAPACITY)
        stat->size = d->capacityLen;
    else
        stat->size = f->kind == REG_FD_ENTRY ? f->entry.size : 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}

/* Serves `body` from where the descriptor stopped. */
static int readBody(reg_fd_t *f, const char *body, size_t len, void *buf,
                    size_t nbyte) {
    size_t n = f->readOff < len ? len - f->readOff : 0;

    if (n > nbyte)
        n = nbyte;
    memcpy(buf, body + f->readOff, n);
    f->readOff += n;
    return (int)n;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    reg_fd_t *f = getFd(d, fd);
    int read;

    if (buf == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;
    if (isDir(f))
        return -EISDIR; /* enumerate a directory with ReadDir */
    if (f->kind == REG_FD_CAPACITY)
        return readBody(f, d->capacity, d->capacityLen, buf, nbyte);
    if (f->kind == REG_FD_BLOB)
        return readBody(f, f->blob, f->blobLen, buf, nbyte);
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
    if (f->kind == REG_FD_SIG || f->kind == REG_FD_LINK) {
        if (f->sigLen + nbyte >
            (f->kind == REG_FD_LINK ? REG_LINK_MAX : sizeof(f->sig)))
            return -EMSGSIZE;
        memcpy(f->sig + f->sigLen, buf, nbyte);
        f->sigLen += nbyte;
        return (int)nbyte;
    }
    if (f->kind == REG_FD_COMMIT) {
        if (f->commitLen + nbyte > sizeof(f->commit))
            return -EMSGSIZE;
        memcpy(f->commit + f->commitLen, buf, nbyte);
        f->commitLen += nbyte;
        return (int)nbyte;
    }
    if (f->kind == REG_FD_LAYER)
        return WantedWriteRegistryLayer(&f->startedWriting, f->writeRef,
                                        f->layer, buf, nbyte);
    /* Only an install accepts bytes; the root and a stored image do not. */
    if (f->kind != REG_FD_WRITE)
        return -EROFS;

    return WantedWriteRegistry(&f->startedWriting, f->writeRef, buf, nbyte);
}

/* Appends one directory entry at `*used` unless it does not fit. */
static bool putDirent(void *buf, size_t bufLen, size_t *used, const char *name,
                      size_t nameLen, uint64_t ino) {
    vfs_dirent_t dir = {0};

    dir.d_ino = ino;
    dir.d_namlen = nameLen;
    dir.d_type = VFS_FILETYPE_REGULAR_FILE;
    dir.d_next = ino + 1;
    /* Report the bytes written. Claiming the whole buffer hands the reader the
     * part of it this never wrote, and a reader that parses that as an entry
     * follows its length and its cookie into nothing. */
    if (*used + sizeof(dir) + nameLen > bufLen)
        return false;
    memcpy((char *)buf + *used, &dir, sizeof(dir));
    *used += sizeof(dir);
    memcpy((char *)buf + *used, name, nameLen);
    *used += nameLen;
    return true;
}

static const char *const ROOT_NAMES[] = {REG_DIR_BLOBS, REG_NODE_CAPACITY,
                                         REG_DIR_IMAGES};
#define ROOT_NAME_COUNT (sizeof(ROOT_NAMES) / sizeof(ROOT_NAMES[0]))

static int readRootDir(void *buf, size_t bufLen, uint64_t *cookie,
                       size_t *bufUsed) {
    size_t used = 0;
    size_t next = (size_t)*cookie;

    for (size_t i = next; i < ROOT_NAME_COUNT; i++) {
        if (!putDirent(buf, bufLen, &used, ROOT_NAMES[i], strlen(ROOT_NAMES[i]),
                       i))
            break;
        next = i + 1;
    }
    *bufUsed = used;
    *cookie = next;
    return 0;
}

static int readBlobsDir(void *buf, size_t bufLen, uint64_t *cookie,
                        size_t *bufUsed) {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    char hex[REGISTRY_META_DIGEST_HEX_LEN + 1];
    size_t used = 0;
    size_t next = (size_t)*cookie;

    /* A blob that goes between two calls shifts the ones after it, so a walk
     * that races an install can skip or repeat a name. */
    for (size_t i = next; RegistryBlobAt(i, digest) == 0; i++) {
        RegistryDigestToHex(digest, hex);
        if (!putDirent(buf, bufLen, &used, hex, REGISTRY_META_DIGEST_HEX_LEN,
                       i))
            break;
        next = i + 1;
    }
    *bufUsed = used;
    *cookie = next;
    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const reg_fd_t *f = getFd(d, fd);
    size_t used = 0;

    if (buf == NULL)
        return -EINVAL;
    if (f == NULL)
        return -EBADF;
    if (f->kind == REG_FD_ROOT)
        return readRootDir(buf, bufLen, cookie, bufUsed);
    if (f->kind == REG_FD_BLOBS)
        return readBlobsDir(buf, bufLen, cookie, bufUsed);
    if (f->kind != REG_FD_IMAGES)
        return -ENOTDIR;

    /* The entry the next call starts from. It advances only over an entry
     * that reached the buffer, so one that did not fit is served next. */
    size_t next = (size_t)*cookie;

    for (size_t i = next; i < d->nEntries; i++) {
        size_t nameLen = strnlen(d->entries[i].name, WAPP_MAX_NAME_LEN);
        size_t verLen = strnlen(d->entries[i].version, WAPP_MAX_VERSION_LEN);
        char ref[REG_REF_MAX];

        memcpy(ref, d->entries[i].name, nameLen);
        ref[nameLen] = VERSION_SEPARATOR;
        memcpy(ref + nameLen + 1, d->entries[i].version, verLen);
        if (!putDirent(buf, bufLen, &used, ref, nameLen + 1 + verLen, i))
            break;
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
    while (path[0] == '/')
        path++;
    if (!takeSegment(path, REG_DIR_IMAGES, &path))
        return -EPERM; /* the root, blobs and capacity are not removable */

    ret = refreshEntries(d);
    if (ret < 0)
        return ret;

    for (size_t i = 0; i < d->nEntries; i++) {
        if (matchesRef(&d->entries[i], path))
            return WantedRegistryRemove(&d->entries[i]);
    }

    return -ENOENT;
}
