/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <vfs.h>
#include <wanted_malloc.h>

#include "dummy-fs.h"

/* ── Constants ──────────────────────────────────────────────────────────── */

#define DUMMY_FS_MAX_NODES 32
#define DUMMY_FS_MAX_FDS 16
#define DUMMY_FS_PATH_LEN 64
#define DUMMY_FS_FILE_SIZE 256

/* Driver-internal fd base — keeps dummy fds out of the 0/1/2 stdio range. */
#define DUMMY_FD_BASE 100

/* ── In-memory store ────────────────────────────────────────────────────── */

typedef enum {
    DUMMY_NODE_NONE = 0,
    DUMMY_NODE_FILE,
    DUMMY_NODE_DIR,
} dummy_node_type_t;

typedef struct {
    dummy_node_type_t type;
    char path[DUMMY_FS_PATH_LEN];
    uint8_t data[DUMMY_FS_FILE_SIZE];
    uint32_t size;
} dummy_node_t;

typedef struct {
    int used;
    int node_idx;
    uint32_t pos;
} dummy_fd_slot_t;

typedef struct {
    dummy_node_t nodes[DUMMY_FS_MAX_NODES];
    dummy_fd_slot_t fds[DUMMY_FS_MAX_FDS];
} dummy_fs_t;

static dummy_fs_t g_dummy_fs;

struct vfs_driver_ctx_t {
    dummy_fs_t *fs;
    bool readonly;
};

/* ── Clock / PRNG state ─────────────────────────────────────────────────── */

#define DUMMY_CLOCK_STEP_NS 1000000ULL /* 1 ms per GetTime call */

static uint64_t g_clock_ns;
static uint32_t g_prng_state = 0xDEAD1234U;

/* ── Fs helpers ─────────────────────────────────────────────────────────── */

static int node_find(dummy_fs_t *fs, const char *path) {
    for (int i = 0; i < DUMMY_FS_MAX_NODES; i++) {
        if (fs->nodes[i].type != DUMMY_NODE_NONE &&
            strncmp(fs->nodes[i].path, path, DUMMY_FS_PATH_LEN) == 0)
            return i;
    }
    return -1;
}

static int node_alloc(dummy_fs_t *fs, const char *path,
                      dummy_node_type_t type) {
    for (int i = 0; i < DUMMY_FS_MAX_NODES; i++) {
        if (fs->nodes[i].type == DUMMY_NODE_NONE) {
            fs->nodes[i].type = type;
            strncpy(fs->nodes[i].path, path, DUMMY_FS_PATH_LEN - 1);
            fs->nodes[i].path[DUMMY_FS_PATH_LEN - 1] = '\0';
            fs->nodes[i].size = 0;
            return i;
        }
    }
    return -1;
}

static int fd_alloc(dummy_fs_t *fs, int node_idx) {
    for (int i = 0; i < DUMMY_FS_MAX_FDS; i++) {
        if (!fs->fds[i].used) {
            fs->fds[i].used = 1;
            fs->fds[i].node_idx = node_idx;
            fs->fds[i].pos = 0;
            return i + DUMMY_FD_BASE;
        }
    }
    return -1;
}

static dummy_fd_slot_t *fd_get(dummy_fs_t *fs, int fd) {
    int idx = fd - DUMMY_FD_BASE;
    if (idx < 0 || idx >= DUMMY_FS_MAX_FDS || !fs->fds[idx].used)
        return NULL;
    return &fs->fds[idx];
}

/* Is node_path a direct (non-nested) child of dir_path? */
static int is_direct_child(const char *dir_path, const char *node_path) {
    size_t dir_len = strlen(dir_path);
    size_t node_len = strlen(node_path);
    if (node_len <= dir_len)
        return 0;
    if (strncmp(node_path, dir_path, dir_len) != 0)
        return 0;
    const char *suffix = node_path + dir_len;
    if (dir_path[dir_len - 1] != '/') {
        if (suffix[0] != '/')
            return 0;
        suffix++;
    }
    return suffix[0] != '\0' && strchr(suffix, '/') == NULL;
}

static void path_join(const char *dir, const char *rel, char *out, size_t len) {
    size_t dir_len = strlen(dir);
    if (dir_len > 0 && dir[dir_len - 1] == '/')
        snprintf(out, len, "%s%s", dir, rel);
    else
        snprintf(out, len, "%s/%s", dir, rel);
}

/* ── VFS driver vtable ──────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    dummy_fs_t *fs = d->fs;
    int ni = node_find(fs, path);

    if (ni < 0) {
        if (!(flags & VFS_O_CREAT))
            return -ENOENT;
        dummy_node_type_t t =
            (flags & VFS_O_DIRECTORY) ? DUMMY_NODE_DIR : DUMMY_NODE_FILE;
        ni = node_alloc(fs, path, t);
        if (ni < 0)
            return -ENOSPC;
    } else {
        if ((flags & VFS_O_CREAT) && (flags & VFS_O_EXCL))
            return -EEXIST;
        if ((flags & VFS_O_TRUNC) && fs->nodes[ni].type == DUMMY_NODE_FILE) {
            fs->nodes[ni].size = 0;
            memset(fs->nodes[ni].data, 0, DUMMY_FS_FILE_SIZE);
        }
    }

    int fd = fd_alloc(fs, ni);
    return fd < 0 ? -EMFILE : fd;
}

static int _OpenAt(vfs_driver_ctx_t d, int dir_drv_fd, const char *rel_path,
                   vfs_oflags_t flags) {
    if (d->readonly && VFS_O_IS_WRITE(flags))
        return -EROFS;
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, dir_drv_fd);
    if (!dfd)
        return -EBADF;
    if (fs->nodes[dfd->node_idx].type != DUMMY_NODE_DIR)
        return -ENOTDIR;

    char abs_path[DUMMY_FS_PATH_LEN];
    path_join(fs->nodes[dfd->node_idx].path, rel_path, abs_path,
              sizeof(abs_path));
    return _Open(d, abs_path, flags);
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    dummy_fs_t *fs = d->fs;
    int idx = fd - DUMMY_FD_BASE;
    if (idx < 0 || idx >= DUMMY_FS_MAX_FDS || !fs->fds[idx].used)
        return -EBADF;
    fs->fds[idx].used = 0;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd || !s)
        return -EBADF;
    dummy_node_t *n = &fs->nodes[dfd->node_idx];
    memset(s, 0, sizeof(*s));
    s->dev = 0x796D6D44U; /* 'Dmmy' */
    s->ino = (uint32_t)dfd->node_idx;
    s->filetype = (n->type == DUMMY_NODE_DIR) ? VFS_FILETYPE_DIRECTORY
                                              : VFS_FILETYPE_REGULAR_FILE;
    s->size = n->size;
    s->nlink = 1;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;
    dummy_node_t *n = &fs->nodes[dfd->node_idx];
    if (n->type == DUMMY_NODE_DIR)
        return -EISDIR;
    uint32_t avail = (dfd->pos < n->size) ? (n->size - dfd->pos) : 0;
    uint32_t to_read = ((uint32_t)nbyte < avail) ? (uint32_t)nbyte : avail;
    if (to_read > 0)
        memcpy(buf, n->data + dfd->pos, to_read);
    dfd->pos += to_read;
    return (int)to_read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (d->readonly)
        return -EROFS;
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;
    dummy_node_t *n = &fs->nodes[dfd->node_idx];
    if (n->type == DUMMY_NODE_DIR)
        return -EISDIR;
    uint32_t avail = DUMMY_FS_FILE_SIZE - dfd->pos;
    uint32_t to_write = ((uint32_t)nbyte < avail) ? (uint32_t)nbyte : avail;
    if (to_write > 0)
        memcpy(n->data + dfd->pos, buf, to_write);
    dfd->pos += to_write;
    if (dfd->pos > n->size)
        n->size = dfd->pos;
    return (int)to_write;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    if (!pos)
        return -EINVAL;
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;
    dummy_node_t *n = &fs->nodes[dfd->node_idx];
    long new_pos;
    switch (whence) {
    case VFS_SEEK_SET:
        new_pos = off;
        break;
    case VFS_SEEK_CUR:
        new_pos = (long)dfd->pos + off;
        break;
    case VFS_SEEK_END:
        new_pos = (long)n->size + off;
        break;
    default:
        return -EINVAL;
    }
    if (new_pos < 0)
        return -EINVAL;
    if (new_pos > DUMMY_FS_FILE_SIZE)
        new_pos = DUMMY_FS_FILE_SIZE;
    dfd->pos = (uint32_t)new_pos;
    *pos = new_pos;
    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (!buf || !cookie || !bufUsed)
        return -EINVAL;
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;
    dummy_node_t *dir = &fs->nodes[dfd->node_idx];
    if (dir->type != DUMMY_NODE_DIR)
        return -ENOTDIR;

    size_t used = 0;
    vfs_dirent_t last;
    memset(&last, 0, sizeof(last));

    for (int i = (int)*cookie; i < DUMMY_FS_MAX_NODES; i++) {
        dummy_node_t *n = &fs->nodes[i];
        if (n->type == DUMMY_NODE_NONE)
            continue;
        if (!is_direct_child(dir->path, n->path))
            continue;

        const char *slash = strrchr(n->path, '/');
        const char *name = slash ? slash + 1 : n->path;
        uint32_t namelen = (uint32_t)strlen(name);

        if (used + sizeof(vfs_dirent_t) + namelen > bufLen)
            break;

        vfs_dirent_t ent;
        memset(&ent, 0, sizeof(ent));
        ent.d_ino = (uint64_t)i;
        ent.d_namlen = namelen;
        ent.d_next = (uint64_t)(i + 1);
        ent.d_type = (n->type == DUMMY_NODE_DIR) ? VFS_FILETYPE_DIRECTORY
                                                 : VFS_FILETYPE_REGULAR_FILE;
        memcpy((uint8_t *)buf + used, &ent, sizeof(vfs_dirent_t));
        memcpy((uint8_t *)buf + used + sizeof(vfs_dirent_t), name, namelen);
        used += sizeof(vfs_dirent_t) + namelen;
        last = ent;
    }

    *bufUsed = used;
    if (used > 0)
        *cookie = last.d_next;
    return 0;
}

static int dummy_rename(dummy_fs_t *fs, int old_fd, const char *old_path,
                        int new_fd, const char *new_path) {
    dummy_fd_slot_t *old_dfd = fd_get(fs, old_fd);
    dummy_fd_slot_t *new_dfd = fd_get(fs, new_fd);
    if (!old_dfd || !new_dfd)
        return -EBADF;

    char abs_old[DUMMY_FS_PATH_LEN];
    char abs_new[DUMMY_FS_PATH_LEN];
    path_join(fs->nodes[old_dfd->node_idx].path, old_path, abs_old,
              sizeof(abs_old));
    path_join(fs->nodes[new_dfd->node_idx].path, new_path, abs_new,
              sizeof(abs_new));

    int ni = node_find(fs, abs_old);
    if (ni < 0)
        return -ENOENT;

    strncpy(fs->nodes[ni].path, abs_new, DUMMY_FS_PATH_LEN - 1);
    fs->nodes[ni].path[DUMMY_FS_PATH_LEN - 1] = '\0';
    return 0;
}

static int _Rename(vfs_driver_ctx_t d, int old_fd, const char *old_path,
                   int new_fd, const char *new_path) {
    if (d->readonly)
        return -EROFS;
    return dummy_rename(d->fs, old_fd, old_path, new_fd, new_path);
}

static int _Mkdir(vfs_driver_ctx_t d, int fd, const char *path) {
    if (d->readonly)
        return -EROFS;
    dummy_fs_t *fs = d->fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;

    char abs_path[DUMMY_FS_PATH_LEN];
    path_join(fs->nodes[dfd->node_idx].path, path, abs_path, sizeof(abs_path));

    if (node_find(fs, abs_path) >= 0)
        return -EEXIST;

    int ni = node_alloc(fs, abs_path, DUMMY_NODE_DIR);
    return ni < 0 ? -ENOSPC : 0;
}

/* ── VfsPlatformFsInit ──────────────────────────────────────────────────── */

static const char id[] = {'D', 'u', 'm', 'y'};

vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *opt,
                                bool readonly) {
    (void)wapp;
    (void)opt;

    vfs_driver_t *drv = WantedMalloc(sizeof(vfs_driver_t));
    if (!drv)
        return NULL;

    struct vfs_driver_ctx_t *ctx =
        WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (!ctx) {
        WantedFree(drv);
        return NULL;
    }
    ctx->fs = &g_dummy_fs;
    ctx->readonly = readonly;

    memset(drv, 0, sizeof(*drv));
    drv->bytesId = *(uint32_t *)(id);
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx = ctx;
    drv->Destroy = _Destroy;
    drv->Open = _Open;
    drv->OpenAt = _OpenAt;
    drv->Close = _Close;
    drv->Stat = _Stat;
    drv->Read = _Read;
    drv->Write = _Write;
    drv->Seek = _Seek;
    drv->ReadDir = _ReadDir;
    drv->Rename = _Rename;
    drv->Mkdir = _Mkdir;
    return drv;
}

/* ── Platform filesystem functions ──────────────────────────────────────── */

int PlatformOpenStateDir(const char *path, bool readonly) {
    if (!path)
        return -EINVAL;
    int ni = node_find(&g_dummy_fs, path);
    if (ni < 0) {
        if (readonly)
            return -ENOENT;
        ni = node_alloc(&g_dummy_fs, path, DUMMY_NODE_DIR);
        if (ni < 0)
            return -ENOSPC;
    }
    int fd = fd_alloc(&g_dummy_fs, ni);
    return fd < 0 ? -EMFILE : fd;
}

const char *PlatformVolumeRoot(void) { return "/data"; }

int PlatformFsRename(int old_fd, const char *old_path, int new_fd,
                     const char *new_path) {
    return dummy_rename(&g_dummy_fs, old_fd, old_path, new_fd, new_path);
}

int PlatformFsMkdir(int fd, const char *path) {
    dummy_fs_t *fs = &g_dummy_fs;
    dummy_fd_slot_t *dfd = fd_get(fs, fd);
    if (!dfd)
        return -EBADF;
    char abs_path[DUMMY_FS_PATH_LEN];
    path_join(fs->nodes[dfd->node_idx].path, path, abs_path, sizeof(abs_path));
    if (node_find(fs, abs_path) >= 0)
        return -EEXIST;
    int ni = node_alloc(fs, abs_path, DUMMY_NODE_DIR);
    return ni < 0 ? -ENOSPC : 0;
}

/* ── Reset ──────────────────────────────────────────────────────────────── */

void DummyFsReset(void) { memset(&g_dummy_fs, 0, sizeof(g_dummy_fs)); }

/* ── Clock / PRNG ───────────────────────────────────────────────────────── */

int PlatformClockGetRes(plat_clk_id_t clk_id, uint64_t *resolution) {
    if (!resolution)
        return -EINVAL;
    if (clk_id != PLAT_CLOCKID_REALTIME && clk_id != PLAT_CLOCKID_MONOTONIC)
        return -EINVAL;
    *resolution = DUMMY_CLOCK_STEP_NS;
    return 0;
}

int PlatformClockGetTime(plat_clk_id_t clk_id, plat_timestamp_t *ts) {
    if (!ts)
        return -EINVAL;
    if (clk_id != PLAT_CLOCKID_REALTIME && clk_id != PLAT_CLOCKID_MONOTONIC)
        return -EINVAL;
    *ts = g_clock_ns;
    g_clock_ns += DUMMY_CLOCK_STEP_NS;
    return 0;
}

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t duration,
                           plat_clk_flags_t flags) {
    (void)clk_id;
    if (flags & PLAT_CLOCK_FLAGS_ABSTIME) {
        if (duration > g_clock_ns)
            g_clock_ns = duration;
    } else {
        g_clock_ns += duration;
    }
    return 0;
}

void DummyClockReset(void) {
    g_clock_ns = 0;
    g_prng_state = 0xDEAD1234U;
}

void DummyClockAdvance(uint64_t ns) { g_clock_ns += ns; }

/* xorshift32 — deterministic, fixed seed. Typo in name is intentional:
 * matches the platform.h declaration. */
int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len == 0)
        return -EINVAL;
    for (size_t i = 0; i < buf_len; i++) {
        g_prng_state ^= g_prng_state << 13;
        g_prng_state ^= g_prng_state >> 17;
        g_prng_state ^= g_prng_state << 5;
        buf[i] = (uint8_t)(g_prng_state & 0xFFU);
    }
    return (int64_t)buf_len;
}
