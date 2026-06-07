#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <cwalk.h>
#include <debug_trace.h>
#include <vfs-devfs.h>
#include <vfs-netfs.h>
#include <vfs-procfs.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Stateless prefix router on top of a single typed-FD table.
 *
 * All opens go through VfsResolvePath before routing, so trailing slashes,
 * '.' and '..' are resolved universally via cwk_path_normalize. A mount table
 * in vfs_ctx_t replaces the hardcoded if/else prefix chain; route_open
 * iterates it for the longest matching prefix. Root VfsReadDir emits
 * mount-table entries after the TarFS phase so /dev, /net and /proc appear in
 * 'ls /'. VfsOpenAt uses the parent fd's stored path to resolve relative
 * paths. */

/* ── Path normalisation ──────────────────────────────────────────────────── */

static int VfsResolvePath(vfs_ctx_t c, int parent_fd, const char *path,
                          char *out, size_t out_size) {
    const char *src = path;
    char combined[VFS_FD_PATH_LEN * 2];

    if (*path != '/') {
        /* Relative path: prepend parent fd's stored path. */
        if (parent_fd < 0 || parent_fd >= VFS_MAX_FDS ||
            c->fds[parent_fd].type == VFS_TYPE_NONE)
            return -EBADF;
        const char *base = c->fds[parent_fd].path;
        size_t base_len = strlen(base);
        size_t path_len = strlen(path);
        if (base_len + 1 + path_len >= sizeof(combined))
            return -ENAMETOOLONG;
        memcpy(combined, base, base_len);
        if (base_len == 0 || base[base_len - 1] != '/')
            combined[base_len++] = '/';
        memcpy(combined + base_len, path, path_len + 1);
        src = combined;
    }

    size_t written = cwk_path_normalize(src, out, out_size);
    if (written >= out_size)
        return -ENAMETOOLONG;

    /* cwk_path_normalize preserves trailing slashes; strip them to match the
     * behaviour callers expect (mount-table prefix matching, FD path storage).
     * Exception: root "/" must not be shortened to "". */
    size_t n = written;
    if (n > 1 && out[n - 1] == '/')
        out[--n] = '\0';

    return 0;
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
    } else if (type == VFS_TYPE_PROC) {
        handle = ProcFs_Open(c, suffix, flags, &open_err);
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
        else if (type == VFS_TYPE_PROC)
            ProcFs_Close(c, handle);
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

    VfsMount(c, "/",     VFS_TYPE_TARFS);
    VfsMount(c, "/dev",  VFS_TYPE_DEV);
    VfsMount(c, "/net",  VFS_TYPE_NET);
    VfsMount(c, "/proc", VFS_TYPE_PROC);

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

/* PLATFORM-type slots can either be preopens (own the driver — Destroy on
 * teardown) or children produced by OpenAt against a preopen (share the
 * parent's driver — close only the host fd). Distinguish via the `driver`
 * pointer being shared with another slot of higher precedence. To keep this
 * simple we Destroy the driver only once: walk the table, dedup driver
 * pointers, and Destroy each unique driver after closing all slots. */
static void DestroyPlatformFds(vfs_ctx_t c) {
    /* First pass: close every host fd. */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (c->fds[i].type != VFS_TYPE_PLATFORM)
            continue;
        const vfs_driver_t *drv = c->fds[i].driver;
        if (drv && drv->Close)
            drv->Close(drv->ctx, c->fds[i].drv_fd);
    }
    /* Second pass: Destroy each unique driver and clear slots. */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (c->fds[i].type != VFS_TYPE_PLATFORM)
            continue;
        const vfs_driver_t *drv = c->fds[i].driver;
        if (drv) {
            /* Clear all slots sharing this driver before Destroy so the
             * second-pass loop won't double-call Destroy. */
            for (int j = i; j < VFS_MAX_FDS; j++) {
                if (c->fds[j].type == VFS_TYPE_PLATFORM &&
                    c->fds[j].driver == drv) {
                    c->fds[j].type = VFS_TYPE_NONE;
                    c->fds[j].driver = NULL;
                    c->fds[j].drv_fd = -1;
                }
            }
            if (drv->Destroy)
                drv->Destroy((vfs_driver_t *)drv);
        } else {
            c->fds[i].type = VFS_TYPE_NONE;
        }
    }
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
        case VFS_TYPE_PROC:
            ProcFs_Close(c, c->fds[i].internal_ctx);
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
    DestroyPlatformFds(c);
}

void VfsDestroy(vfs_ctx_t *c) {
    if (NULL == c || NULL == *c)
        return;
    DestroyTypedFds(*c);
    DevFs_Destroy(*c);
    NetFs_Destroy(*c);
    ProcFs_Destroy(*c);
    DestroyStreamFd(*c, VFS_STDERR);
    DestroyStreamFd(*c, VFS_STDOUT);
    DestroyStreamFd(*c, VFS_STDIN);
    if ((*c)->tarfs)
        TarFsDestroy((*c)->tarfs);

    WantedFree(*c);
    *c = NULL;
}

void VfsSetPrivileged(vfs_ctx_t c, bool privileged) {
    if (c)
        c->privileged = privileged;
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

    /* stdio slots 0/1/2 equal the POSIX native fds — platform drivers use drv_fd
     * directly as the native fd; non-platform drivers (log, null) ignore it. */
    c->fds[slot].type = VFS_TYPE_STREAM;
    c->fds[slot].driver = driver;
    c->fds[slot].drv_fd = slot;
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

    /* PLATFORM parent: bypass the mount-table router. Relative paths resolve
     * against the host directory fd directly via the driver's OpenAt. */
    if (fd >= 0 && fd < VFS_MAX_FDS &&
        c->fds[fd].type == VFS_TYPE_PLATFORM && path[0] != '/') {
        const vfs_driver_t *drv = c->fds[fd].driver;
        if (!drv || !drv->OpenAt)
            return -ENOTSUP;
        int host_fd = drv->OpenAt(drv->ctx, c->fds[fd].drv_fd, path, flags);
        if (host_fd < 0)
            return host_fd;

        int new_slot = FindFirstClosedFd(c);
        if (new_slot < 0) {
            if (drv->Close)
                drv->Close(drv->ctx, host_fd);
            return new_slot;
        }
        c->fds[new_slot].type = VFS_TYPE_PLATFORM;
        c->fds[new_slot].driver = drv;
        c->fds[new_slot].drv_fd = host_fd;
        c->fds[new_slot].flags = flags;
        /* Store the wapp-visible absolute path so subsequent relative OpenAt
         * resolutions (rare for files; common for subdirs) work cleanly. */
        const char *parent = c->fds[fd].path;
        size_t plen = strlen(parent);
        size_t slen = strlen(path);
        if (plen + 1 + slen < VFS_FD_PATH_LEN) {
            memcpy(c->fds[new_slot].path, parent, plen);
            c->fds[new_slot].path[plen] = '/';
            memcpy(c->fds[new_slot].path + plen + 1, path, slen + 1);
        } else {
            c->fds[new_slot].path[0] = '\0';
        }
        return new_slot;
    }

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
    case VFS_TYPE_PROC:
        r = ProcFs_Close(c, c->fds[fd].internal_ctx);
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
    case VFS_TYPE_PLATFORM: {
        const vfs_driver_t *drv = c->fds[fd].driver;
        /* Preopen slots (those registered via VfsBindPlatformFd) are torn down
         * by VfsDestroy — closing them mid-wapp would orphan the preopen and
         * break Zig's openDirAbsolute lookup. Detect a preopen by checking if
         * the slot's path matches a registered preopen path. Simpler: if drv
         * is shared with another live PLATFORM slot, this one is a child fd
         * created by OpenAt and safe to close fully. */
        bool is_preopen = false;
        for (int i = 0; i < VFS_MAX_FDS; i++) {
            if (i != fd && c->fds[i].type == VFS_TYPE_PLATFORM &&
                c->fds[i].driver == drv) {
                /* Driver shared — this slot is a child; safe to close. */
                is_preopen = false;
                goto close_platform;
            }
        }
        /* No other slot uses this driver: this is the preopen itself. Keep it
         * open so the wapp can continue to find the preopen via prestat. */
        is_preopen = true;
    close_platform:
        if (is_preopen)
            return 0;
        r = (drv && drv->Close) ? drv->Close(drv->ctx, c->fds[fd].drv_fd) : 0;
        c->fds[fd].type = VFS_TYPE_NONE;
        c->fds[fd].driver = NULL;
        c->fds[fd].drv_fd = -1;
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
    case VFS_TYPE_PROC:
        return ProcFs_Stat(c, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_TARFS:
        return TarFs_Stat(c->tarfs, c->fds[fd].internal_ctx, stat);
    case VFS_TYPE_STREAM:
    case VFS_TYPE_PLATFORM:
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
    case VFS_TYPE_PROC:
        return ProcFs_Read(c, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_TARFS:
        return TarFs_Read(c->tarfs, c->fds[fd].internal_ctx, buf, nbyte);
    case VFS_TYPE_STREAM:
    case VFS_TYPE_PLATFORM:
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
    case VFS_TYPE_PLATFORM:
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
    case VFS_TYPE_PLATFORM:
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
    if (!CheckFd(c, fd))
        return -EBADF;
    if (NULL == buf || NULL == cookie || NULL == bufUsed)
        return -EINVAL;

    /* Trace AFTER the NULL guard so the test cases that probe NULL inputs
     * don't dereference here. */
    DEBUG_TRACE("%d (%zu) %llu", fd, bufLen, *cookie);

    switch (c->fds[fd].type) {
    case VFS_TYPE_DEV:
        return DevFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen, cookie,
                             bufUsed);
    case VFS_TYPE_NET:
        return NetFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen, cookie,
                             bufUsed);
    case VFS_TYPE_PROC:
        return ProcFs_ReadDir(c, c->fds[fd].internal_ctx, buf, bufLen, cookie,
                              bufUsed);
    case VFS_TYPE_PLATFORM:
        return TRY_DRV(c->fds[fd].driver, ReadDir, c->fds[fd].drv_fd, buf,
                       bufLen, cookie, bufUsed);
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

int VfsBindPlatformFd(vfs_ctx_t c, const char *path,
                      const vfs_driver_t *driver, int host_fd) {
    if (!c || !path || !driver)
        return -EINVAL;

    int slot = FindFirstClosedFd(c);
    if (slot < 0)
        return slot;

    c->fds[slot].type = VFS_TYPE_PLATFORM;
    c->fds[slot].driver = driver;
    c->fds[slot].drv_fd = host_fd;
    c->fds[slot].flags = VFS_O_RDWR | VFS_O_DIRECTORY;
    size_t plen = strlen(path);
    if (plen >= VFS_FD_PATH_LEN)
        plen = VFS_FD_PATH_LEN - 1;
    memcpy(c->fds[slot].path, path, plen);
    c->fds[slot].path[plen] = '\0';
    DEBUG_TRACE("preopen %s -> fd=%d (host_fd=%d)", path, slot, host_fd);
    return slot;
}

int VfsRename(vfs_ctx_t c, int old_fd, const char *old_path,
              int new_fd, const char *new_path) {
    DEBUG_TRACE("%d, %s -> %d, %s", old_fd, old_path, new_fd, new_path);

    if (!CheckFd(c, old_fd) || !CheckFd(c, new_fd))
        return -EBADF;
    if (!old_path || !new_path)
        return -EINVAL;

    /* Both fds must reach the same writable backing store. Only PLATFORM
     * (preopen-backed host fs) supports rename today; TarFS is read-only,
     * ProcFS is read-only, NetFS doesn't define directory semantics. */
    if (c->fds[old_fd].type != c->fds[new_fd].type)
        return -EXDEV;

    switch (c->fds[old_fd].type) {
    case VFS_TYPE_TARFS:
    case VFS_TYPE_PROC:
        return -EROFS;
    case VFS_TYPE_NET:
        return -EINVAL;
    case VFS_TYPE_PLATFORM: {
        const vfs_driver_t *old_drv = c->fds[old_fd].driver;
        const vfs_driver_t *new_drv = c->fds[new_fd].driver;
        if (old_drv != new_drv)
            return -EXDEV;
        if (!old_drv || !old_drv->Rename)
            return -ENOSYS;
        return old_drv->Rename(old_drv->ctx, c->fds[old_fd].drv_fd, old_path,
                               c->fds[new_fd].drv_fd, new_path);
    }
    default:
        return -ENOTSUP;
    }
}

int VfsMkdir(vfs_ctx_t c, int fd, const char *path) {
    DEBUG_TRACE("%d, %s", fd, path);

    if (!CheckFd(c, fd))
        return -EBADF;
    if (!path)
        return -EINVAL;

    switch (c->fds[fd].type) {
    case VFS_TYPE_TARFS:
    case VFS_TYPE_PROC:
        return -EROFS;
    case VFS_TYPE_NET:
        return -EINVAL;
    case VFS_TYPE_PLATFORM: {
        const vfs_driver_t *drv = c->fds[fd].driver;
        if (!drv || !drv->Mkdir)
            return -ENOSYS;
        return drv->Mkdir(drv->ctx, c->fds[fd].drv_fd, path);
    }
    default:
        return -ENOTSUP;
    }
}
