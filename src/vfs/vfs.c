#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-devfs.h>
#include <vfs-netfs.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Stateless prefix router on top of a single typed-FD table.
 *
 * VfsOpen / VfsOpenAt detect "/dev/" and "/net/" prefixes and route those
 * paths through DevFs / NetFs. When the wapp has a tarfs context
 * (`c->tarfs != NULL`), every other path resolves against the layered TAR
 * index. Stdio is preregistered into c->fds[0..2] as VFS_TYPE_STREAM during
 * VfsRegister, and dispatched through the embedded driver pointer. TARFS owns
 * root, the prefix router owns /dev and /net, and everything else is
 * unsupported. */

static inline bool path_has_prefix(const char *path, const char *prefix) {
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static inline bool path_is_routed_prefix(const char *path) {
    return path_has_prefix(path, "/dev/") || path_has_prefix(path, "/net/");
}

static inline bool CheckFd(struct vfs_ctx_t *c, int fd) {
    return c && fd >= 0 && fd < VFS_MAX_FDS &&
           c->fds[fd].type != VFS_TYPE_NONE;
}

static int FindFirstClosedFd(struct vfs_ctx_t *c) {
    if (!c)
        return -EINVAL;

    for (int i = ROOT_FD; i < VFS_MAX_FDS; i++) {
        if (c->fds[i].type == VFS_TYPE_NONE)
            return i;
    }
    return -EMFILE;
}

static int route_open(vfs_ctx_t c, const char *path, vfs_oflags_t flags) {
    int fd;
    void *handle;
    vfs_fd_type_t type;
    int open_err = -ENOENT;

    if (path_has_prefix(path, "/dev/")) {
        type = VFS_TYPE_DEV;
        handle = DevFs_Open(c, path + 5, flags, &open_err);
    } else if (path_has_prefix(path, "/net/")) {
        type = VFS_TYPE_NET;
        handle = NetFs_Open(c, path + 5, flags, &open_err);
    } else if (c->tarfs != NULL) {
        /* TarFs is read-only; reject write-mode opens before touching the
         * index so callers get -EROFS rather than a generic -ENOENT. */
        if ((flags & 03) != VFS_O_RDONLY ||
            (flags & (VFS_O_CREAT | VFS_O_TRUNC))) {
            return -EROFS;
        }
        type = VFS_TYPE_TARFS;
        handle = TarFs_Open(c->tarfs, path, flags);
        /* TarFs_Open returns NULL for ENOENT or OOM; both surface as ENOENT to
         * the caller — OOM is indistinguishable without an API change. */
    } else {
        return -ENOTSUP;
    }

    if (handle == NULL) {
        return open_err;
    }

    fd = FindFirstClosedFd(c);
    if (fd < 0) {
        if (type == VFS_TYPE_DEV)
            DevFs_Close(c, handle);
        else if (type == VFS_TYPE_NET)
            NetFs_Close(c, handle);
        else
            TarFs_Close(c->tarfs, handle);
        return fd;
    }

    c->fds[fd].type = type;
    c->fds[fd].internal_ctx = handle;
    c->fds[fd].flags = flags;
    return fd;
}

/* PUBLIC INTERFACE */

vfs_ctx_t VfsInit() {
    struct vfs_ctx_t *c;

    c = (struct vfs_ctx_t *)WantedMalloc(sizeof(*c));
    if (!c)
        return c;

    memset(c, 0, sizeof(*c));

    return c;
}

static void DestroyStreamFd(vfs_ctx_t c, unsigned fd) {
    if (c->fds[fd].type != VFS_TYPE_STREAM)
        return;
    const vfs_driver_t *drv = c->fds[fd].driver;
    if (drv) {
        if (drv->Close)
            drv->Close(drv->ctx, c->fds[fd].drv_fd);
        if (drv->Destroy)
            drv->Destroy((vfs_driver_t *)drv);
    }
    c->fds[fd].type = VFS_TYPE_NONE;
    c->fds[fd].driver = NULL;
}

static void DestroyTypedFds(vfs_ctx_t c) {
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        switch (c->fds[i].type) {
        case VFS_TYPE_DEV:
            DevFs_Close(c, c->fds[i].internal_ctx);
            break;
        case VFS_TYPE_NET:
            NetFs_Close(c, c->fds[i].internal_ctx);
            break;
        case VFS_TYPE_TARFS:
            TarFs_Close(c->tarfs, c->fds[i].internal_ctx);
            break;
        default:
            continue;
        }
        c->fds[i].type = VFS_TYPE_NONE;
        c->fds[i].internal_ctx = NULL;
    }
}

void VfsDestroy(vfs_ctx_t *c) {
    if (NULL == c || NULL == *c)
        return;
    DestroyTypedFds(*c);
    DevFs_Destroy(*c);
    NetFs_Destroy(*c);
    DestroyStreamFd(*c, VFS_STDERR);
    DestroyStreamFd(*c, VFS_STDOUT);
    DestroyStreamFd(*c, VFS_STDIN);
    if ((*c)->tarfs)
        TarFsDestroy((*c)->tarfs);

    WantedFree(*c);
    *c = NULL;
}

int VfsAttachTarfs(vfs_ctx_t c, vfs_tarfs_ctx_t *tarfs) {
    if (!c)
        return -EINVAL;
    if (c->tarfs && c->tarfs != tarfs)
        TarFsDestroy(c->tarfs);
    c->tarfs = tarfs;
    return 0;
}

/* VfsRegister wires up stdio only. TARFS owns root and the prefix router owns
 * /dev and /net. Callers who hand us anything else lose the driver they
 * built; the registry layer in vfs-wanted-ctrl.c filters those before they
 * reach here, but we destroy defensively so a stale call can't leak. */
int VfsRegister(vfs_ctx_t c, const char *path, const vfs_driver_t *driver) {
    if (NULL == driver || NULL == c)
        return -EINVAL;

    DEBUG_TRACE("%s (%.4s)", path, driver->id);

    int slot;
    int open_flags;

    if (strcmp(path, "<stdin>") == 0) {
        slot = VFS_STDIN;
        open_flags = VFS_O_RDONLY;
    } else if (strcmp(path, "<stdout>") == 0) {
        slot = VFS_STDOUT;
        open_flags = VFS_O_WRONLY;
    } else if (strcmp(path, "<stderr>") == 0) {
        slot = VFS_STDERR;
        open_flags = VFS_O_WRONLY;
    } else {
        if (driver->Destroy)
            driver->Destroy((vfs_driver_t *)driver);
        return -EINVAL;
    }

    int drv_fd = TRY_DRV(driver, Open, path, open_flags);
    c->fds[slot].type = VFS_TYPE_STREAM;
    c->fds[slot].driver = driver;
    c->fds[slot].drv_fd = drv_fd;
    c->fds[slot].flags = open_flags;
    return 0;
}

int VfsOpen(vfs_ctx_t c, const char *path, vfs_oflags_t flags) {
    DEBUG_TRACE("%s (0x%x)", path, flags);

    if (!c || !path || *path == '\0')
        return -EINVAL;

    if (path_is_routed_prefix(path) || c->tarfs != NULL)
        return route_open(c, path, flags);

    return -ENOENT;
}

int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, vfs_oflags_t flags) {
    DEBUG_TRACE("%d, %s (0x%x)", fd, path, flags);

    if (!c || NULL == path || *path == '\0')
        return -EINVAL;

    /* Only absolute routed paths are supported — a relative path against a
     * typed-FD parent has no sensible mapping under DevFs/NetFs/TARFS. */
    if (path_is_routed_prefix(path) || c->tarfs != NULL)
        return route_open(c, path, flags);

    return -ENOTSUP;
}

int VfsClose(vfs_ctx_t c, int fd) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    int r;
    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        r = DevFs_Close(c, c->fds[fd].internal_ctx);
        break;
    case VFS_TYPE_NET:
        r = NetFs_Close(c, c->fds[fd].internal_ctx);
        break;
    case VFS_TYPE_TARFS:
        r = TarFs_Close(c->tarfs, c->fds[fd].internal_ctx);
        break;
    case VFS_TYPE_STREAM: {
        const vfs_driver_t *drv = c->fds[fd].driver;
        r = (drv && drv->Close) ? drv->Close(drv->ctx, c->fds[fd].drv_fd)
                                : 0;
        /* Stream slots are preopens — VfsDestroy still owns the driver. */
        return r;
    }
    default:
        return -EBADF;
    }
    c->fds[fd].type = VFS_TYPE_NONE;
    c->fds[fd].internal_ctx = NULL;
    return r;
}

int VfsStatAt(vfs_ctx_t c, int fd, const char *path, vfs_stat_t *stat) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    /* The prefix router serves absolute paths directly; fd is used only
     * to inherit the VFS context, not as a directory handle. */
    int newfd = VfsOpenAt(c, fd, path, 0);
    if (newfd < 0)
        return newfd;

    int ret = VfsStat(c, newfd, stat);
    VfsClose(c, newfd);
    return ret;
}

int VfsStat(vfs_ctx_t c, int fd, vfs_stat_t *stat) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_Stat(c, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_NET:
        return NetFs_Stat(c, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_TARFS:
        return TarFs_Stat(c->tarfs, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_STREAM:
        return TRY_DRV(c->fds[fd].driver, Stat, c->fds[fd].drv_fd, stat);
    default:
        return -EBADF;
    }
}

int VfsStatSet(vfs_ctx_t c, int fd, vfs_stat_t stat) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_StatSet(c, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_TARFS:
        return -EROFS;
    case VFS_TYPE_STREAM:
        return TRY_DRV(c->fds[fd].driver, StatSet, c->fds[fd].drv_fd, stat);
    default:
        return -ENOTSUP;
    }
}

int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte) {
    DEBUG_TRACE("%d, %zu", fd, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == buf)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_Read(c, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_NET:
        return NetFs_Read(c, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_TARFS:
        return TarFs_Read(c->tarfs, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_STREAM:
        return TRY_DRV(c->fds[fd].driver, Read, c->fds[fd].drv_fd, buf, nbyte);
    default:
        return -EBADF;
    }
}

int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte) {
    DEBUG_TRACE("%d, %zu", fd, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == buf)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_Write(c, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_NET:
        return NetFs_Write(c, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_TARFS:
        return -EROFS;
    case VFS_TYPE_STREAM:
        return TRY_DRV(c->fds[fd].driver, Write, c->fds[fd].drv_fd, buf, nbyte);
    default:
        return -EBADF;
    }
}

int VfsSeek(vfs_ctx_t c, int fd, long off, vfs_whence_t whence, long *pos) {
    DEBUG_TRACE("%d, %ld, %d", fd, off, whence);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == pos)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_Seek(c, c->fds[fd].internal_ctx, off, whence, pos);
    case VFS_TYPE_TARFS:
        return TarFs_Seek(c->tarfs, c->fds[fd].internal_ctx, off, whence, pos);
    case VFS_TYPE_STREAM:
        return TRY_DRV(c->fds[fd].driver, Seek, c->fds[fd].drv_fd, off, whence,
                       pos);
    default:
        return -ENOTSUP;
    }
}

int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie,
               size_t *bufUsed) {
    DEBUG_TRACE("%d (%zu) %llu", fd, bufLen, *cookie);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == buf || NULL == cookie || NULL == bufUsed)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen, cookie,
                             bufUsed);
    case VFS_TYPE_TARFS:
        return TarFs_ReadDir(c->tarfs, c->fds[fd].internal_ctx, buf, bufLen,
                             cookie, bufUsed);
    default:
        return -ENOTSUP;
    }
}

int VfsUnlink(vfs_ctx_t c, int fd, const char *path) {
    DEBUG_TRACE("%d (%s)", fd, path);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == path)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_Unlink(c, c->fds[fd].internal_ctx, path);
    case VFS_TYPE_TARFS:
        return -EROFS;
    default:
        return -ENOTSUP;
    }
}

int VfsSockAccept(vfs_ctx_t c, int fd, vfs_oflags_t flags, int *newFd) {
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (c->fds[fd].type == VFS_TYPE_NET)
        return NetFs_SockAccept(c, c->fds[fd].internal_ctx, flags, newFd);
    return -ENOTSOCK;
}

int VfsSockRecv(vfs_ctx_t c, int fd, void *buf, size_t nbyte,
                vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    DEBUG_TRACE("%d (0x%x) %zu", fd, iflags, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (c->fds[fd].type == VFS_TYPE_NET)
        return NetFs_SockRecv(c, c->fds[fd].internal_ctx, buf, nbyte, iflags,
                              oflags);
    return -ENOTSOCK;
}

int VfsSockSend(vfs_ctx_t c, int fd, const void *buf, size_t nbyte,
                vfs_sdflags_t flags) {
    DEBUG_TRACE("%d (0x%x) %zu", fd, flags, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (c->fds[fd].type == VFS_TYPE_NET)
        return NetFs_SockSend(c, c->fds[fd].internal_ctx, buf, nbyte, flags);
    return -ENOTSOCK;
}

int VfsSockShutdown(vfs_ctx_t c, int fd, vfs_sdflags_t flags) {
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (c->fds[fd].type == VFS_TYPE_NET)
        return NetFs_SockShutdown(c, c->fds[fd].internal_ctx, flags);
    return -ENOTSOCK;
}
