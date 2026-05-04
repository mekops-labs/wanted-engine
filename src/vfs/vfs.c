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
 * All opens go through VfsResolvePath + PathNormalize before routing, so
 * trailing slashes, '.' and '..' are resolved universally. A mount table in
 * vfs_ctx_t replaces the hardcoded if/else prefix chain; route_open iterates
 * it for the longest matching prefix. Root VfsReadDir emits mount-table
 * entries after the TarFS phase so /dev, /net and /proc appear in 'ls /'.
 * VfsOpenAt uses the parent fd's stored path to resolve relative paths. */

/* ── Path normalisation ──────────────────────────────────────────────────── */

static int PathNormalize(const char *in, char *out, size_t out_size) {
    char tmp[VFS_FD_PATH_LEN];
    size_t in_len = strlen(in);
    if (in_len >= VFS_FD_PATH_LEN)
        return -ENAMETOOLONG;
    memcpy(tmp, in, in_len + 1);

    const char *segs[VFS_FD_PATH_LEN / 2];
    int depth = 0;

    char *p = tmp;
    while (*p == '/')
        p++;

    while (*p) {
        char *seg = p;
        while (*p && *p != '/')
            p++;
        if (*p == '/')
            *p++ = '\0';
        while (*p == '/')
            p++;

        if (seg[0] == '\0' || (seg[0] == '.' && seg[1] == '\0')) {
            /* skip */
        } else if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            if (depth > 0)
                depth--;
        } else {
            segs[depth++] = seg;
        }
    }

    if (depth == 0) {
        if (out_size < 2)
            return -ENAMETOOLONG;
        out[0] = '/';
        out[1] = '\0';
        return 0;
    }

    size_t pos = 0;
    for (int i = 0; i < depth; i++) {
        size_t slen = strlen(segs[i]);
        if (pos + 1 + slen >= out_size)
            return -ENAMETOOLONG;
        out[pos++] = '/';
        memcpy(out + pos, segs[i], slen);
        pos += slen;
    }
    out[pos] = '\0';
    return 0;
}

static int VfsResolvePath(vfs_ctx_t c, int parent_fd, const char *path,
                          char *out, size_t out_size) {
    if (*path == '/')
        return PathNormalize(path, out, out_size);

    /* Relative path: prepend parent fd's stored path. */
    if (parent_fd < 0 || parent_fd >= VFS_MAX_FDS ||
        c->fds[parent_fd].type == VFS_TYPE_NONE)
        return -EBADF;

    char combined[VFS_FD_PATH_LEN * 2];
    const char *base = c->fds[parent_fd].path;
    size_t base_len = strlen(base);
    size_t path_len = strlen(path);

    if (base_len + 1 + path_len >= sizeof(combined))
        return -ENAMETOOLONG;

    memcpy(combined, base, base_len);
    if (base_len == 0 || base[base_len - 1] != '/')
        combined[base_len++] = '/';
    memcpy(combined + base_len, path, path_len + 1);

    return PathNormalize(combined, out, out_size);
}

/* ── Mount table ─────────────────────────────────────────────────────────── */

static int VfsMount(vfs_ctx_t c, const char *prefix, vfs_fd_type_t type) {
    if (c->mounts_cnt >= VFS_MAX_MOUNTS)
        return -ENOSPC;
    size_t plen = strlen(prefix);
    if (plen >= sizeof(c->mounts[0].prefix))
        return -ENAMETOOLONG;
    vfs_mount_t *m = &c->mounts[c->mounts_cnt++];
    memcpy(m->prefix, prefix, plen + 1);
    m->type = type;
    return 0;
}

/* ── Flat-directory readdir helper ───────────────────────────────────────── */

int VfsFlatDirReadDir(const vfs_dir_entry_t *entries, size_t count, void *buf,
                      size_t bufLen, uint64_t *cookie, size_t *bufUsed) {
    size_t used = 0;
    uint64_t idx = *cookie;

    for (; idx < count; idx++) {
        size_t namlen = strlen(entries[idx].name);
        if (used + sizeof(vfs_dirent_t) + namlen > bufLen)
            break;
        vfs_dirent_t dir = {0};
        dir.d_ino = idx;
        dir.d_namlen = (uint32_t)namlen;
        dir.d_type = entries[idx].type;
        dir.d_next = used + sizeof(vfs_dirent_t) + namlen;
        memcpy((uint8_t *)buf + used, &dir, sizeof(dir));
        memcpy((uint8_t *)buf + used + sizeof(dir), entries[idx].name, namlen);
        used += sizeof(dir) + namlen;
    }

    *cookie = idx;
    *bufUsed = used;
    return 0;
}

/* ── Internal helpers ────────────────────────────────────────────────────── */

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

/* ── Route open (mount-table dispatch) ───────────────────────────────────── */

static int route_open(vfs_ctx_t c, const char *path, vfs_oflags_t flags) {
    /* Longest-prefix match against the mount table. */
    vfs_fd_type_t type = VFS_TYPE_NONE;
    size_t best_len = 0;

    for (uint8_t i = 0; i < c->mounts_cnt; i++) {
        size_t plen = strlen(c->mounts[i].prefix);
        bool matches;
        if (plen == 1) {
            /* "/" matches any absolute path */
            matches = (path[0] == '/');
        } else {
            matches = strncmp(path, c->mounts[i].prefix, plen) == 0 &&
                      (path[plen] == '/' || path[plen] == '\0');
        }
        if (matches && plen > best_len) {
            best_len = plen;
            type = c->mounts[i].type;
        }
    }

    if (type == VFS_TYPE_NONE)
        return -ENOENT;

    /* Suffix passed to subsystems: skip mount prefix and any leading slash. */
    const char *suffix = path + best_len;
    if (*suffix == '/')
        suffix++;

    void *handle;
    int open_err = -ENOENT;

    if (type == VFS_TYPE_DEV) {
        handle = DevFs_Open(c, suffix, flags, &open_err);
    } else if (type == VFS_TYPE_NET) {
        handle = NetFs_Open(c, suffix, flags, &open_err);
    } else if (type == VFS_TYPE_TARFS) {
        if (c->tarfs == NULL)
            return -ENOENT;
        if ((flags & 03) != VFS_O_RDONLY ||
            (flags & (VFS_O_CREAT | VFS_O_TRUNC))) {
            return -EROFS;
        }
        handle = TarFs_Open(c->tarfs, path, flags);
    } else {
        return -ENOTSUP;
    }

    if (handle == NULL)
        return open_err;

    int fd = FindFirstClosedFd(c);
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
    strncpy(c->fds[fd].path, path, VFS_FD_PATH_LEN - 1);
    c->fds[fd].path[VFS_FD_PATH_LEN - 1] = '\0';
    return fd;
}

/* ── PUBLIC INTERFACE ────────────────────────────────────────────────────── */

vfs_ctx_t VfsInit() {
    struct vfs_ctx_t *c;

    c = (struct vfs_ctx_t *)WantedMalloc(sizeof(*c));
    if (!c)
        return c;

    memset(c, 0, sizeof(*c));

    VfsMount(c, "/",    VFS_TYPE_TARFS);
    VfsMount(c, "/dev", VFS_TYPE_DEV);
    VfsMount(c, "/net", VFS_TYPE_NET);
    /* /proc is mounted by Phase 3 (ProcFS) when its handler is available. */

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

/* VfsRegister wires up stdio only. TARFS owns root and the mount table owns
 * /dev, /net, /proc. Callers who hand us anything else lose the driver they
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

    char norm[VFS_FD_PATH_LEN];
    int r = VfsResolvePath(c, -1, path, norm, sizeof(norm));
    if (r < 0)
        return r;

    return route_open(c, norm, flags);
}

int VfsOpenAt(vfs_ctx_t c, int fd, const char *path, vfs_oflags_t flags) {
    DEBUG_TRACE("%d, %s (0x%x)", fd, path, flags);

    if (!c || NULL == path || *path == '\0')
        return -EINVAL;

    char norm[VFS_FD_PATH_LEN];
    int r = VfsResolvePath(c, fd, path, norm, sizeof(norm));
    if (r < 0)
        return r;

    return route_open(c, norm, flags);
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

/* Cookie high bit separates TarFS phase (bit=0) from mount-table phase (bit=1)
 * during root directory listing. TarFS never produces cookies with bit 63 set
 * so the spaces don't overlap. */
#define MOUNT_PHASE_BIT (UINT64_C(1) << 63)

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
    case VFS_TYPE_NET:
        return NetFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen, cookie,
                             bufUsed);
    case VFS_TYPE_TARFS: {
        /* Non-root subdirectory: delegate entirely to TarFS. */
        if (c->fds[fd].path[0] != '/' || c->fds[fd].path[1] != '\0') {
            return TarFs_ReadDir(c->tarfs, c->fds[fd].internal_ctx, buf, bufLen,
                                 cookie, bufUsed);
        }

        /* Root fd: TarFS phase first, then mount-table phase.
         *
         * WASI fd_readdir contract: bufUsed < bufLen means "directory
         * exhausted". We must therefore emit mount entries in the SAME call
         * that TarFS returns its final batch, appending to the unused tail of
         * the buffer. We only hand off to a pure mount phase on subsequent
         * calls where cookie already has MOUNT_PHASE_BIT set. */
        if (!(*cookie & MOUNT_PHASE_BIT)) {
            int r = TarFs_ReadDir(c->tarfs, c->fds[fd].internal_ctx, buf,
                                  bufLen, cookie, bufUsed);
            if (r < 0)
                return r;
            if (*bufUsed == bufLen)
                return 0; /* buffer full — TarFS may have more entries */
            /* TarFS done in this segment: append mount entries to the
             * remaining buffer space and switch to mount phase. */
            *cookie = MOUNT_PHASE_BIT;
        }

        /* Mount-table phase: append after any existing content. */
        uint64_t idx = *cookie & ~MOUNT_PHASE_BIT;
        uint8_t *mbuf = (uint8_t *)buf + *bufUsed;
        size_t mspace = bufLen - *bufUsed;
        size_t mused = 0;
        for (; idx < c->mounts_cnt; idx++) {
            const char *pfx = c->mounts[idx].prefix;
            if (pfx[1] == '\0')
                continue; /* skip "/" */
            const char *name = pfx + 1;
            if (strchr(name, '/') != NULL)
                continue; /* skip nested mounts, emit only top-level */
            size_t namlen = strlen(name);
            if (mused + sizeof(vfs_dirent_t) + namlen > mspace)
                break;
            vfs_dirent_t dir = {0};
            dir.d_ino = (uint64_t)(0x10000 + idx);
            dir.d_namlen = (uint32_t)namlen;
            dir.d_type = VFS_FILETYPE_DIRECTORY;
            dir.d_next = MOUNT_PHASE_BIT | (idx + 1);
            memcpy(mbuf + mused, &dir, sizeof(dir));
            memcpy(mbuf + mused + sizeof(dir), name, namlen);
            mused += sizeof(dir) + namlen;
        }
        *cookie = MOUNT_PHASE_BIT | idx;
        *bufUsed += mused;
        return 0;
    }
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
