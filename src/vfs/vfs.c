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

#include <cwalk.h>

/* Phase 4/5 — stateless prefix router.
 *
 * VfsOpen / VfsOpenAt detect "/dev/" and "/net/" prefixes and route those
 * paths through DevFs / NetFs. Phase 5 adds the TARFS branch: when the wapp
 * has a tarfs context (`c->tarfs != NULL`), every other path resolves
 * against the layered TAR index. All routed opens return into the typed-FD
 * table (c->fds[]); the legacy per-driver fildes[]/rootDriver dispatch
 * remains as a fallback for wapps without a tarfs context, until Phase 6
 * rewires the boot path and Phase 8 deletes the legacy plumbing.
 *
 * The two FD pools share one integer space — allocators consult both arrays
 * to keep VFS-fd numbers monotone, as required by the WASI fd-prestat
 * contract. */

static inline bool is_typed_fd(struct vfs_ctx_t *c, int fd) {
    return c && fd >= 0 && fd < MAX_OPEN &&
           c->fds[fd].type != VFS_TYPE_NONE;
}

static inline bool CheckFd(struct vfs_ctx_t *c, int fd) {
    if (!c || fd < 0 || fd >= MAX_OPEN)
        return false;
    if (c->fds[fd].type != VFS_TYPE_NONE)
        return true;
    if (c->fildes[fd].drv == NULL)
        return false;

    return true;
}

static int FindFirstClosedFd(struct vfs_ctx_t *c) {
    if (!c)
        return -EINVAL;

    for (int i = ROOT_FD; i < MAX_OPEN; i++) {
        if (!c->fildes[i].opened && c->fds[i].type == VFS_TYPE_NONE) {
            return i;
        }
    }
    return -EMFILE;
}

/* Typed FDs (DevFs/NetFs/TarFs) skip ROOT_FD when it has been claimed by a
 * legacy "/" preopen via VfsRegister. WASI's prestat contract pins ROOT_FD
 * to the root mount; only legacy "/" opens may land there. */
static int FindFirstClosedTypedFd(struct vfs_ctx_t *c) {
    if (!c)
        return -EINVAL;

    int start = (c->fildes[ROOT_FD].drv != NULL) ? (ROOT_FD + 1) : ROOT_FD;
    for (int i = start; i < MAX_OPEN; i++) {
        if (!c->fildes[i].opened && c->fds[i].type == VFS_TYPE_NONE) {
            return i;
        }
    }
    return -EMFILE;
}

static bool path_has_prefix(const char *path, const char *prefix) {
    return strncmp(path, prefix, strlen(prefix)) == 0;
}

static int route_open(vfs_ctx_t c, const char *path, vfs_oflags_t flags) {
    int fd;
    void *handle;
    vfs_fd_type_t type;

    if (path_has_prefix(path, "/dev/")) {
        type = VFS_TYPE_DEV;
        handle = DevFs_Open(c, path + 5, flags);
    } else if (path_has_prefix(path, "/net/")) {
        type = VFS_TYPE_NET;
        handle = NetFs_Open(c, path + 5, flags);
    } else if (c->tarfs != NULL) {
        type = VFS_TYPE_TARFS;
        handle = TarFs_Open(c->tarfs, path, flags);
    } else {
        return -ENOTSUP;
    }

    if (handle == NULL) {
        return -ENOENT;
    }

    fd = FindFirstClosedTypedFd(c);
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

static inline bool path_is_routed_prefix(const char *path) {
    return path_has_prefix(path, "/dev/") || path_has_prefix(path, "/net/");
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

static void DestroyRootDriver(vfs_ctx_t c) {
    if (!c->rootDriver)
        return;

    if (c->rootDriver->Destroy != NULL)
        c->rootDriver->Destroy((vfs_driver_t *)c->rootDriver);
}

static void DestroyFildesDrv(vfs_ctx_t c, unsigned fd) {
    if (c->fildes[fd].drv && c->fildes[fd].drv->Destroy)
        c->fildes[fd].drv->Destroy((vfs_driver_t *)c->fildes[fd].drv);
}

static void DestroyTypedFds(vfs_ctx_t c) {
    for (int i = 0; i < MAX_OPEN; i++) {
        if (c->fds[i].type == VFS_TYPE_DEV) {
            DevFs_Close(c, c->fds[i].internal_ctx);
        } else if (c->fds[i].type == VFS_TYPE_NET) {
            NetFs_Close(c, c->fds[i].internal_ctx);
        } else if (c->fds[i].type == VFS_TYPE_TARFS) {
            TarFs_Close(c->tarfs, c->fds[i].internal_ctx);
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
    DestroyRootDriver(*c);
    DestroyFildesDrv(*c, VFS_STDERR);
    DestroyFildesDrv(*c, VFS_STDOUT);
    DestroyFildesDrv(*c, VFS_STDIN);

    WantedFree(*c);
    *c = NULL;
}

int VfsRegister(vfs_ctx_t c, const char *path, const vfs_driver_t *driver) {
    int ret = 0;
    DEBUG_TRACE("%s (%.4s)", path, driver->id);

    if (NULL == driver || NULL == c) {
        return -EINVAL;
    }

    struct cwk_segment seg;

    if (memcmp("/", path, 2) == 0) {
        c->fildes[ROOT_FD].drv = driver;
        ret = TRY_DRV(c->fildes[ROOT_FD].drv, Register, path, driver);
        if (ret < 0)
            return ret;
        c->rootDriver = driver;
    } else if (memcmp("<stdin>", path, 8) == 0) {
        c->fildes[VFS_STDIN].drv = driver;
        c->fildes[VFS_STDIN].drv_fd = TRY_DRV(driver, Open, path, VFS_O_RDONLY);
        c->fildes[VFS_STDIN].opened = true;
    } else if (memcmp("<stdout>", path, 9) == 0) {
        c->fildes[VFS_STDOUT].drv = driver;
        c->fildes[VFS_STDOUT].drv_fd =
            TRY_DRV(driver, Open, path, VFS_O_WRONLY);
        ;
        c->fildes[VFS_STDOUT].opened = true;
    } else if (memcmp("<stderr>", path, 9) == 0) {
        c->fildes[VFS_STDERR].drv = driver;
        c->fildes[VFS_STDERR].drv_fd =
            TRY_DRV(driver, Open, path, VFS_O_WRONLY);
        ;
        c->fildes[VFS_STDERR].opened = true;
    } else {
        if (!c->rootDriver) {
            return -EINVAL;
        }

        if (!cwk_path_get_first_segment(path, &seg)) {
            return -EINVAL;
        }

        ret = TRY_DRV(c->rootDriver, Register, seg.begin, driver);
        if (ret < 0)
            return ret;
    }

    return ret;
}

int VfsOpen(vfs_ctx_t c, const char *path, vfs_oflags_t flags) {
    DEBUG_TRACE("%s (0x%x)", path, flags);

    if (!c || !path || *path == '\0') {
        return -EINVAL;
    }

    if (path_is_routed_prefix(path) || c->tarfs != NULL) {
        return route_open(c, path, flags);
    }

    return VfsOpenAt(c, ROOT_FD, path, flags);
}

int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, vfs_oflags_t flags) {
    DEBUG_TRACE("%d, %s (0x%x)", fd, path, flags);

    if (!c || NULL == path || *path == '\0') {
        return -EINVAL;
    }

    /* Absolute routed paths short-circuit into the router, regardless of the
     * parent fd. /dev/ and /net/ always route; arbitrary paths route when the
     * wapp has a tarfs context. */
    if (path_is_routed_prefix(path) || c->tarfs != NULL) {
        return route_open(c, path, flags);
    }

    if (!CheckFd(c, fd))
        return -EBADF;

    /* Phase 4 does not yet support OpenAt on typed-FD parents. The legacy
     * preopen entries (ROOT_FD etc.) still go through the legacy dispatch. */
    if (is_typed_fd(c, fd)) {
        return -ENOTSUP;
    }

    int f = FindFirstClosedFd(c);
    if (f < 0)
        return f;

    int newFd =
        TRY_DRV(c->fildes[fd].drv, OpenAt, c->fildes[fd].drv_fd, path, flags);
    if (newFd < 0) {
        return newFd;
    }

    c->fildes[f].drv = c->fildes[fd].drv;
    c->fildes[f].drv_fd = newFd;
    c->fildes[f].flags = flags;
    c->fildes[f].opened = true;

    return f;
}

int VfsClose(vfs_ctx_t c, int fd) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (is_typed_fd(c, fd)) {
        int r;
        if (c->fds[fd].type == VFS_TYPE_DEV) {
            r = DevFs_Close(c, c->fds[fd].internal_ctx);
        } else if (c->fds[fd].type == VFS_TYPE_NET) {
            r = NetFs_Close(c, c->fds[fd].internal_ctx);
        } else if (c->fds[fd].type == VFS_TYPE_TARFS) {
            r = TarFs_Close(c->tarfs, c->fds[fd].internal_ctx);
        } else {
            r = -EBADF;
        }
        c->fds[fd].type = VFS_TYPE_NONE;
        c->fds[fd].internal_ctx = NULL;
        return r;
    }

    c->fildes[fd].opened = false;

    return TRY_DRV(c->fildes[fd].drv, Close, c->fildes[fd].drv_fd);
}

int VfsStatAt(vfs_ctx_t c, int fd, const char *path, vfs_stat_t *stat) {
    int ret, f;

    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (is_typed_fd(c, fd)) {
        return -ENOTSUP;
    }

    f = TRY_DRV(c->fildes[fd].drv, OpenAt, c->fildes[fd].drv_fd, path, 0);
    if (f < 0) {
        return f;
    }

    ret = TRY_DRV(c->fildes[fd].drv, Stat, f, stat);
    if (ret < 0) {
        return ret;
    }

    ret = TRY_DRV(c->fildes[fd].drv, Close, f);

    return ret;
}

int VfsStat(vfs_ctx_t c, int fd, vfs_stat_t *stat) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_Stat(c, c->fds[fd].internal_ctx, stat);
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_Stat(c, c->fds[fd].internal_ctx, stat);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return TarFs_Stat(c->tarfs, c->fds[fd].internal_ctx, stat);
        return -EBADF;
    }

    return TRY_DRV(c->fildes[fd].drv, Stat, c->fildes[fd].drv_fd, stat);
}

int VfsStatSet(vfs_ctx_t c, int fd, vfs_stat_t stat) {
    DEBUG_TRACE("%d", fd);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_StatSet(c, c->fds[fd].internal_ctx, stat);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return -EROFS;
        return -ENOTSUP;
    }

    return TRY_DRV(c->fildes[fd].drv, StatSet, c->fildes[fd].drv_fd, stat);
}

int VfsRead(vfs_ctx_t c, int fd, void *buf, size_t nbyte) {
    DEBUG_TRACE("%d, %zu", fd, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (NULL == buf) {
        return -EINVAL;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_Read(c, c->fds[fd].internal_ctx, buf, nbyte);
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_Read(c, c->fds[fd].internal_ctx, buf, nbyte);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return TarFs_Read(c->tarfs, c->fds[fd].internal_ctx, buf, nbyte);
        return -EBADF;
    }

    return TRY_DRV(c->fildes[fd].drv, Read, c->fildes[fd].drv_fd, buf, nbyte);
}

int VfsWrite(vfs_ctx_t c, int fd, const void *buf, size_t nbyte) {
    DEBUG_TRACE("%d, %zu", fd, nbyte);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (NULL == buf) {
        return -EINVAL;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_Write(c, c->fds[fd].internal_ctx, buf, nbyte);
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_Write(c, c->fds[fd].internal_ctx, buf, nbyte);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return -EROFS;
        return -EBADF;
    }

    return TRY_DRV(c->fildes[fd].drv, Write, c->fildes[fd].drv_fd, buf, nbyte);
}

int VfsSeek(vfs_ctx_t c, int fd, long off, vfs_whence_t whence, long *pos) {
    DEBUG_TRACE("%d, %ld, %d", fd, off, whence);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (NULL == pos) {
        return -EINVAL;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_Seek(c, c->fds[fd].internal_ctx, off, whence, pos);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return TarFs_Seek(c->tarfs, c->fds[fd].internal_ctx, off, whence,
                              pos);
        return -ENOTSUP;
    }

    return TRY_DRV(c->fildes[fd].drv, Seek, c->fildes[fd].drv_fd, off, whence,
                   pos);
}

int VfsReadDir(vfs_ctx_t c, int fd, void *buf, size_t bufLen, uint64_t *cookie,
               size_t *bufUsed) {
    DEBUG_TRACE("%d (%zu) %llu", fd, bufLen, *cookie);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (NULL == buf || NULL == cookie || NULL == bufUsed) {
        return -EINVAL;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen,
                                 cookie, bufUsed);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return TarFs_ReadDir(c->tarfs, c->fds[fd].internal_ctx, buf,
                                 bufLen, cookie, bufUsed);
        return -ENOTSUP;
    }

    return TRY_DRV(c->fildes[fd].drv, ReadDir, c->fildes[fd].drv_fd, buf,
                   bufLen, cookie, bufUsed);
}

int VfsUnlink(vfs_ctx_t c, int fd, const char *path) {
    DEBUG_TRACE("%d (%s)", fd, path);

    if (!CheckFd(c, fd))
        return -EBADF;

    if (NULL == path) {
        return -EINVAL;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_DEV)
            return DevFs_Unlink(c, c->fds[fd].internal_ctx, path);
        if (c->fds[fd].type == VFS_TYPE_TARFS)
            return -EROFS;
        return -ENOTSUP;
    }

    return TRY_DRV(c->fildes[fd].drv, Unlink, c->fildes[fd].drv_fd, path);
}

int VfsSockAccept(vfs_ctx_t c, int fd, vfs_oflags_t flags, int *newFd) {
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd)) {
        return -EBADF;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_SockAccept(c, c->fds[fd].internal_ctx, flags, newFd);
        return -ENOTSOCK;
    }

    return TRY_DRV(c->fildes[fd].drv, SockAccept, c->fildes[fd].drv_fd, flags,
                   newFd);
}

int VfsSockRecv(vfs_ctx_t c, int fd, void *buf, size_t nbyte,
                vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    DEBUG_TRACE("%d (0x%x) %zu", fd, iflags, nbyte);

    if (!CheckFd(c, fd)) {
        return -EBADF;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_SockRecv(c, c->fds[fd].internal_ctx, buf, nbyte,
                                  iflags, oflags);
        return -ENOTSOCK;
    }

    return TRY_DRV(c->fildes[fd].drv, SockRecv, c->fildes[fd].drv_fd, buf,
                   nbyte, iflags, oflags);
}

int VfsSockSend(vfs_ctx_t c, int fd, const void *buf, size_t nbyte,
                vfs_sdflags_t flags) {
    DEBUG_TRACE("%d (0x%x) %zu", fd, flags, nbyte);

    if (!CheckFd(c, fd)) {
        return -EBADF;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_SockSend(c, c->fds[fd].internal_ctx, buf, nbyte,
                                  flags);
        return -ENOTSOCK;
    }

    return TRY_DRV(c->fildes[fd].drv, SockSend, c->fildes[fd].drv_fd, buf,
                   nbyte, flags);
}

int VfsSockShutdown(vfs_ctx_t c, int fd, vfs_sdflags_t flags) {
    DEBUG_TRACE("%d (0x%x)", fd, flags);

    if (!CheckFd(c, fd)) {
        return -EBADF;
    }

    if (is_typed_fd(c, fd)) {
        if (c->fds[fd].type == VFS_TYPE_NET)
            return NetFs_SockShutdown(c, c->fds[fd].internal_ctx, flags);
        return -ENOTSOCK;
    }

    return TRY_DRV(c->fildes[fd].drv, SockShutdown, c->fildes[fd].drv_fd,
                   flags);
}
