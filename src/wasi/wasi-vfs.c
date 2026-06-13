/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <wasm_export.h>

/* wanted includes */
#include <platform.h>
#include <vfs.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>
#include <wasi.h>

#include "wasi_types.h"

typedef struct wasi_iovec_t {
    uint32_t buf;     /* wasm linear-memory offset */
    uint32_t buf_len;
} wasi_iovec_t;

#define CASE_RET(e1, e2)                                                       \
    case e1:                                                                   \
        return e2;                                                             \
        break

static __wasi_errno_t errno_to_wasi(int errnum) {
    switch (-errnum) {
        CASE_RET(EPERM, __WASI_ERRNO_PERM);
        CASE_RET(ENOENT, __WASI_ERRNO_NOENT);
        CASE_RET(ESRCH, __WASI_ERRNO_SRCH);
        CASE_RET(EINTR, __WASI_ERRNO_INTR);
        CASE_RET(EIO, __WASI_ERRNO_IO);
        CASE_RET(ENXIO, __WASI_ERRNO_NXIO);
        CASE_RET(E2BIG, __WASI_ERRNO_2BIG);
        CASE_RET(ENOEXEC, __WASI_ERRNO_NOEXEC);
        CASE_RET(EBADF, __WASI_ERRNO_BADF);
        CASE_RET(ECHILD, __WASI_ERRNO_CHILD);
        CASE_RET(EAGAIN, __WASI_ERRNO_AGAIN);
        CASE_RET(ENOMEM, __WASI_ERRNO_NOMEM);
        CASE_RET(EACCES, __WASI_ERRNO_ACCES);
        CASE_RET(EFAULT, __WASI_ERRNO_FAULT);
        CASE_RET(EBUSY, __WASI_ERRNO_BUSY);
        CASE_RET(EEXIST, __WASI_ERRNO_EXIST);
        CASE_RET(EXDEV, __WASI_ERRNO_XDEV);
        CASE_RET(ENODEV, __WASI_ERRNO_NODEV);
        CASE_RET(ENOTDIR, __WASI_ERRNO_NOTDIR);
        CASE_RET(EISDIR, __WASI_ERRNO_ISDIR);
        CASE_RET(EINVAL, __WASI_ERRNO_INVAL);
        CASE_RET(ENFILE, __WASI_ERRNO_NFILE);
        CASE_RET(EMFILE, __WASI_ERRNO_MFILE);
        CASE_RET(ENOTTY, __WASI_ERRNO_NOTTY);
        CASE_RET(ETXTBSY, __WASI_ERRNO_TXTBSY);
        CASE_RET(EFBIG, __WASI_ERRNO_FBIG);
        CASE_RET(ENOSPC, __WASI_ERRNO_NOSPC);
        CASE_RET(ENOSYS, __WASI_ERRNO_NOSYS);
        CASE_RET(ESPIPE, __WASI_ERRNO_SPIPE);
        CASE_RET(EROFS, __WASI_ERRNO_ROFS);
        CASE_RET(EMLINK, __WASI_ERRNO_MLINK);
        CASE_RET(EPIPE, __WASI_ERRNO_PIPE);
        CASE_RET(EDOM, __WASI_ERRNO_DOM);
        CASE_RET(ERANGE, __WASI_ERRNO_RANGE);
        CASE_RET(ENAMETOOLONG, __WASI_ERRNO_NAMETOOLONG);
        CASE_RET(EDESTADDRREQ, __WASI_ERRNO_DESTADDRREQ);
        CASE_RET(ECONNREFUSED, __WASI_ERRNO_CONNREFUSED);
        CASE_RET(ECONNABORTED, __WASI_ERRNO_CONNABORTED);
        CASE_RET(ECONNRESET, __WASI_ERRNO_CONNRESET);
    }
    return __WASI_ERRNO_INVAL;
}

/* Validate a wasm offset+length and convert to a native pointer.
 * Returns NULL for off==0 (NULL-equivalent guard) or any invalid range. */
static inline void *vaddr(wasm_exec_env_t e, uint32_t off, uint32_t len) {
    if (off == 0)
        return NULL;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(e);
    if (!wasm_runtime_validate_app_addr(inst, (uint64_t)off, (uint64_t)len))
        return NULL;
    return wasm_runtime_addr_app_to_native(inst, (uint64_t)off);
}

static inline wasi_ctx_t *get_ctx(wasm_exec_env_t e) {
    return (wasi_ctx_t *)wasm_runtime_get_user_data(e);
}

/*
 * WASI API implementation
 */

static int32_t wasi_args_get(wasm_exec_env_t exec_env,
                             int32_t argv_app, int32_t argv_buf_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    uint32_t *argv = (uint32_t *)vaddr(exec_env, argv_app,
                                       ctx->argc * sizeof(uint32_t));
    if (!argv && ctx->argc > 0)
        return __WASI_ERRNO_FAULT;

    char *argv_buf = (char *)vaddr(exec_env, argv_buf_app, 1);
    /* argv_buf may legitimately be writable with size we don't yet know — full
     * boundary checking happens per-character below via validate_app_addr. */
    if (!argv_buf && ctx->argc > 0)
        return __WASI_ERRNO_FAULT;

    uint32_t buf_off = (uint32_t)argv_buf_app;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    for (uint32_t i = 0; i < ctx->argc; ++i) {
        size_t len = strlen(ctx->argv[i]);
        if (!wasm_runtime_validate_app_addr(inst, buf_off, len + 1))
            return __WASI_ERRNO_FAULT;
        char *dst = wasm_runtime_addr_app_to_native(inst, buf_off);
        argv[i] = buf_off;
        memcpy(dst, ctx->argv[i], len);
        dst[len] = '\0';
        buf_off += (uint32_t)(len + 1);
    }
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_args_sizes_get(wasm_exec_env_t exec_env,
                                   int32_t argc_app, int32_t argv_buf_size_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_size_t *argc = vaddr(exec_env, argc_app, sizeof(__wasi_size_t));
    __wasi_size_t *argv_buf_size =
        vaddr(exec_env, argv_buf_size_app, sizeof(__wasi_size_t));
    if (!argc || !argv_buf_size)
        return __WASI_ERRNO_FAULT;

    __wasi_size_t buf_len = 0;
    for (uint32_t i = 0; i < ctx->argc; ++i)
        buf_len += (__wasi_size_t)(strlen(ctx->argv[i]) + 1);

    *argc = ctx->argc;
    *argv_buf_size = buf_len;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_environ_get(wasm_exec_env_t exec_env,
                                int32_t env_app, int32_t env_buf_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    uint32_t *env = (uint32_t *)vaddr(exec_env, env_app,
                                      ctx->envc * sizeof(uint32_t));
    if (!env && ctx->envc > 0)
        return __WASI_ERRNO_FAULT;

    char *env_buf = (char *)vaddr(exec_env, env_buf_app, 1);
    /* env_buf size is unknown here; each entry is bounds-checked per-character
     * below via validate_app_addr (same pattern as wasi_args_get). */
    if (!env_buf && ctx->envc > 0)
        return __WASI_ERRNO_FAULT;

    uint32_t buf_off = (uint32_t)env_buf_app;
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    for (uint32_t i = 0; i < ctx->envc; ++i) {
        size_t len = strlen(ctx->envp[i]);
        if (!wasm_runtime_validate_app_addr(inst, buf_off, len + 1))
            return __WASI_ERRNO_FAULT;
        char *dst = wasm_runtime_addr_app_to_native(inst, buf_off);
        env[i] = buf_off;
        memcpy(dst, ctx->envp[i], len);
        dst[len] = '\0';
        buf_off += (uint32_t)(len + 1);
    }
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_environ_sizes_get(wasm_exec_env_t exec_env,
                                      int32_t env_count_app,
                                      int32_t env_buf_size_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_size_t *env_count =
        vaddr(exec_env, env_count_app, sizeof(__wasi_size_t));
    __wasi_size_t *env_buf_size =
        vaddr(exec_env, env_buf_size_app, sizeof(__wasi_size_t));
    if (!env_count || !env_buf_size)
        return __WASI_ERRNO_FAULT;

    __wasi_size_t buf_len = 0;
    for (uint32_t i = 0; i < ctx->envc; ++i)
        buf_len += (__wasi_size_t)(strlen(ctx->envp[i]) + 1);

    *env_count = ctx->envc;
    *env_buf_size = buf_len;
    return __WASI_ERRNO_SUCCESS;
}

/* Lazily resolve preopens[fd] to a real VFS fd. The 4 standard entries
 * (stdin/stdout/stderr/root) start with fd=-1; the first prestat_get on the
 * root preopen triggers a VfsOpen("/"). Engine-injected preopens added via
 * WasiCtxAddPreopen are eagerly bound and skip this path. */
static wasi_preopen_t *resolve_preopen(wasi_ctx_t *ctx, int fd) {
    for (uint8_t i = 0; i < ctx->preopens_cnt; i++) {
        wasi_preopen_t *p = &ctx->preopens[i];
        if (p->fd == fd)
            return p;
        if (p->fd == -1) {
            /* Lazy entry — bind now. */
            int host_fd = VfsOpen(ctx->vfsCtx, p->path,
                                  VFS_O_RDONLY | VFS_O_DIRECTORY);
            if (host_fd < 0)
                continue;
            p->fd = host_fd;
            if (p->fd == fd)
                return p;
        }
    }
    return NULL;
}

static int32_t wasi_fd_prestat_dir_name(wasm_exec_env_t exec_env,
                                        int32_t fd, int32_t path_app,
                                        int32_t path_len) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    wasi_preopen_t *p = resolve_preopen(ctx, fd);
    if (!p)
        return __WASI_ERRNO_BADF;

    char *path = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!path)
        return __WASI_ERRNO_FAULT;

    size_t slen = strlen(p->path) + 1;
    memcpy(path, p->path, slen < (size_t)path_len ? slen : (size_t)path_len);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_prestat_get(wasm_exec_env_t exec_env,
                                   int32_t fd, int32_t buf_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    /* stdin/stdout/stderr live as STREAM slots at fds 0–2 and are not preopens
     * from the wapp's perspective — Zig iterates from fd=3. Reject explicitly
     * so a buggy iterator doesn't get a misleading SUCCESS. */
    if (fd < 3)
        return __WASI_ERRNO_BADF;

    wasi_preopen_t *p = resolve_preopen(ctx, fd);
    if (!p)
        return __WASI_ERRNO_BADF;

    uint8_t *buf = vaddr(exec_env, buf_app, 8);
    if (!buf)
        return __WASI_ERRNO_FAULT;

    *(uint32_t *)(buf + 0) = __WASI_PREOPENTYPE_DIR;
    *(uint32_t *)(buf + 4) = (uint32_t)(strlen(p->path) + 1);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_fdstat_get(wasm_exec_env_t exec_env,
                                  int32_t fd, int32_t fdstat_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_fdstat_t *fdstat =
        vaddr(exec_env, fdstat_app, sizeof(__wasi_fdstat_t));
    if (!fdstat)
        return __WASI_ERRNO_FAULT;

    vfs_stat_t stat;
    int ret = VfsStat(ctx->vfsCtx, fd, &stat);
    if (ret < 0)
        return errno_to_wasi(ret);

    fdstat->fs_filetype = stat.filetype;
    fdstat->fs_flags = stat.oflags;
    fdstat->fs_rights_base = (uint64_t)-1;
    fdstat->fs_rights_inheriting = fdstat->fs_rights_base;

    /* Make descriptors 0,1,2 look like a TTY */
    if (fd <= 2) {
        fdstat->fs_rights_base &=
            ~(__WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL);
    }
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_fdstat_set_flags(wasm_exec_env_t exec_env,
                                        int32_t fd, int32_t flags) {
    (void)exec_env; (void)fd; (void)flags;
    return __WASI_ERRNO_SUCCESS;
}

/* WASI unstable maps whence values 0/1/2 to CUR/END/SET (legacy ABI). */
static int32_t wasi_unstable_fd_seek(wasm_exec_env_t exec_env,
                                     int32_t fd, int64_t offset,
                                     int32_t wasi_whence, int32_t result_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_filesize_t *result =
        vaddr(exec_env, result_app, sizeof(__wasi_filesize_t));
    if (!result)
        return __WASI_ERRNO_FAULT;

    int whence;
    switch (wasi_whence) {
    case 0: whence = VFS_SEEK_CUR; break;
    case 1: whence = VFS_SEEK_END; break;
    case 2: whence = VFS_SEEK_SET; break;
    default: return __WASI_ERRNO_INVAL;
    }

    long pos;
    int ret = VfsSeek(ctx->vfsCtx, fd, offset, whence, &pos);
    if (ret < 0)
        return errno_to_wasi(ret);
    *result = (__wasi_filesize_t)pos;
    return __WASI_ERRNO_SUCCESS;
}

/* WASI snapshot_preview1 maps whence 0/1/2 to SET/CUR/END. */
static int32_t wasi_preview1_fd_seek(wasm_exec_env_t exec_env,
                                     int32_t fd, int64_t offset,
                                     int32_t wasi_whence, int32_t result_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_filesize_t *result =
        vaddr(exec_env, result_app, sizeof(__wasi_filesize_t));
    if (!result)
        return __WASI_ERRNO_FAULT;

    int whence;
    switch (wasi_whence) {
    case 0: whence = VFS_SEEK_SET; break;
    case 1: whence = VFS_SEEK_CUR; break;
    case 2: whence = VFS_SEEK_END; break;
    default: return __WASI_ERRNO_INVAL;
    }

    long pos;
    int ret = VfsSeek(ctx->vfsCtx, fd, offset, whence, &pos);
    if (ret < 0)
        return errno_to_wasi(ret);
    *result = (__wasi_filesize_t)pos;
    return __WASI_ERRNO_SUCCESS;
}

static void write_filestat(uint8_t *buf, const vfs_stat_t *statbuf) {
    memset(buf, 0, 64);
    *(uint64_t *)(buf +  0) = statbuf->dev;
    *(uint64_t *)(buf +  8) = statbuf->ino;
    *(uint8_t  *)(buf + 16) = statbuf->filetype;
    *(uint64_t *)(buf + 24) = statbuf->nlink;
    *(uint64_t *)(buf + 32) = statbuf->size;
    *(uint64_t *)(buf + 40) = statbuf->atim;
    *(uint64_t *)(buf + 48) = statbuf->mtim;
    *(uint64_t *)(buf + 56) = statbuf->ctim;
}

static int32_t wasi_fd_filestat_get(wasm_exec_env_t exec_env,
                                    int32_t fd, int32_t buf_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    uint8_t *buf = vaddr(exec_env, buf_app, 64);
    if (!buf)
        return __WASI_ERRNO_FAULT;

    vfs_stat_t statbuf;
    int ret = VfsStat(ctx->vfsCtx, fd, &statbuf);
    if (ret < 0)
        return errno_to_wasi(ret);

    write_filestat(buf, &statbuf);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_path_filestat_get(wasm_exec_env_t exec_env,
                                      int32_t fd, int32_t flags,
                                      int32_t path_app, int32_t path_len,
                                      int32_t buf_app) {
    (void)flags;
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (path_len < 0 || path_len >= 512)
        return __WASI_ERRNO_INVAL;

    char *path = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!path && path_len > 0)
        return __WASI_ERRNO_FAULT;
    uint8_t *buf = vaddr(exec_env, buf_app, 64);
    if (!buf)
        return __WASI_ERRNO_FAULT;

    char host_path[513];
    if (path_len > 0)
        memcpy(host_path, path, (size_t)path_len);
    host_path[path_len] = '\0';

    const char *stat_path = (path_len == 0) ? "." : host_path;
    int vfd = VfsOpenAt(ctx->vfsCtx, fd, stat_path,
                        VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (vfd < 0)
        vfd = VfsOpenAt(ctx->vfsCtx, fd, stat_path, VFS_O_RDONLY);
    if (vfd < 0)
        return errno_to_wasi(vfd);

    vfs_stat_t statbuf;
    int ret = VfsStat(ctx->vfsCtx, vfd, &statbuf);
    VfsClose(ctx->vfsCtx, vfd);
    if (ret < 0)
        return errno_to_wasi(ret);

    write_filestat(buf, &statbuf);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_path_open(wasm_exec_env_t exec_env,
                              int32_t dirfd, int32_t dirflags,
                              int32_t path_app, int32_t path_len,
                              int32_t oflags,
                              int64_t fs_rights_base,
                              int64_t fs_rights_inheriting,
                              int32_t fs_flags,
                              int32_t fd_app) {
    (void)dirflags; (void)fs_rights_inheriting;
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (path_len < 0 || path_len >= 512)
        return __WASI_ERRNO_INVAL;

    char *path = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!path && path_len > 0)
        return __WASI_ERRNO_FAULT;
    __wasi_fd_t *fd_out = vaddr(exec_env, fd_app, sizeof(__wasi_fd_t));
    if (!fd_out)
        return __WASI_ERRNO_FAULT;

    char host_path[513];
    if (path_len > 0)
        memcpy(host_path, path, (size_t)path_len);
    host_path[path_len] = '\0';

    int flags = ((oflags & __WASI_OFLAGS_CREAT) ? VFS_O_CREAT : 0) |
                ((oflags & __WASI_OFLAGS_DIRECTORY) ? VFS_O_DIRECTORY : 0) |
                ((oflags & __WASI_OFLAGS_EXCL) ? VFS_O_EXCL : 0) |
                ((oflags & __WASI_OFLAGS_TRUNC) ? VFS_O_TRUNC : 0) |
                ((fs_flags & __WASI_FDFLAGS_APPEND) ? VFS_O_APPEND : 0) |
                ((fs_flags & __WASI_FDFLAGS_DSYNC) ? VFS_O_DSYNC : 0) |
                ((fs_flags & __WASI_FDFLAGS_NONBLOCK) ? VFS_O_NONBLOCK : 0) |
                ((fs_flags & __WASI_FDFLAGS_RSYNC) ? VFS_O_RSYNC : 0) |
                ((fs_flags & __WASI_FDFLAGS_SYNC) ? VFS_O_SYNC : 0);

    if ((fs_rights_base & __WASI_RIGHTS_FD_READ) &&
        (fs_rights_base & __WASI_RIGHTS_FD_WRITE)) {
        flags |= VFS_O_RDWR;
    } else if (fs_rights_base & __WASI_RIGHTS_FD_WRITE) {
        flags |= VFS_O_WRONLY;
    } else if (fs_rights_base & __WASI_RIGHTS_FD_READ) {
        flags |= VFS_O_RDONLY;
    }

    const char *open_path = (path_len == 0) ? "." : host_path;
    int host_fd = VfsOpenAt(ctx->vfsCtx, dirfd, open_path, flags);
    if (host_fd < 0)
        return errno_to_wasi(host_fd);

    *fd_out = (__wasi_fd_t)host_fd;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_path_unlink_file(wasm_exec_env_t exec_env,
                                     int32_t fd, int32_t path_app,
                                     int32_t path_len) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (path_len < 0 || path_len >= 512)
        return __WASI_ERRNO_INVAL;

    char *path = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!path && path_len > 0)
        return __WASI_ERRNO_FAULT;

    char host_path[513];
    if (path_len > 0)
        memcpy(host_path, path, (size_t)path_len);
    host_path[path_len] = '\0';

    int ret = VfsUnlink(ctx->vfsCtx, fd, host_path);
    if (ret < 0)
        return errno_to_wasi(ret);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_path_rename(wasm_exec_env_t exec_env,
                                int32_t old_fd,
                                int32_t old_path_app, int32_t old_path_len,
                                int32_t new_fd,
                                int32_t new_path_app, int32_t new_path_len) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (old_path_len < 0 || old_path_len >= 512 ||
        new_path_len < 0 || new_path_len >= 512)
        return __WASI_ERRNO_INVAL;

    char *op = vaddr(exec_env, old_path_app, (uint32_t)old_path_len);
    char *np = vaddr(exec_env, new_path_app, (uint32_t)new_path_len);
    if ((!op && old_path_len > 0) || (!np && new_path_len > 0))
        return __WASI_ERRNO_FAULT;

    char old_buf[513];
    char new_buf[513];
    if (old_path_len > 0)
        memcpy(old_buf, op, (size_t)old_path_len);
    old_buf[old_path_len] = '\0';
    if (new_path_len > 0)
        memcpy(new_buf, np, (size_t)new_path_len);
    new_buf[new_path_len] = '\0';

    int ret = VfsRename(ctx->vfsCtx, old_fd, old_buf, new_fd, new_buf);
    if (ret < 0)
        return errno_to_wasi(ret);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_path_create_directory(wasm_exec_env_t exec_env,
                                          int32_t fd, int32_t path_app,
                                          int32_t path_len) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (path_len < 0 || path_len >= 512)
        return __WASI_ERRNO_INVAL;

    char *p = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!p && path_len > 0)
        return __WASI_ERRNO_FAULT;

    char host_path[513];
    if (path_len > 0)
        memcpy(host_path, p, (size_t)path_len);
    host_path[path_len] = '\0';

    int ret = VfsMkdir(ctx->vfsCtx, fd, host_path);
    if (ret < 0)
        return errno_to_wasi(ret);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_read(wasm_exec_env_t exec_env,
                            int32_t fd, int32_t iovs_app,
                            int32_t iovs_len, int32_t nread_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (iovs_len < 0)
        return __WASI_ERRNO_INVAL;

    wasi_iovec_t *iovs =
        vaddr(exec_env, iovs_app, (uint32_t)iovs_len * sizeof(wasi_iovec_t));
    __wasi_size_t *nread = vaddr(exec_env, nread_app, sizeof(__wasi_size_t));
    if (!nread)
        return __WASI_ERRNO_FAULT;
    if (!iovs && iovs_len > 0)
        return __WASI_ERRNO_FAULT;

    ssize_t res = 0;
    for (int32_t i = 0; i < iovs_len; i++) {
        uint32_t off = iovs[i].buf;
        uint32_t len = iovs[i].buf_len;
        if (len == 0)
            continue;
        void *addr = vaddr(exec_env, off, len);
        if (!addr)
            return __WASI_ERRNO_FAULT;

        int ret = VfsRead(ctx->vfsCtx, fd, addr, len);
        if (ret < 0)
            return errno_to_wasi(ret);
        res += ret;
        if ((uint32_t)ret < len)
            break;
    }
    *nread = (__wasi_size_t)res;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_write(wasm_exec_env_t exec_env,
                             int32_t fd, int32_t iovs_app,
                             int32_t iovs_len, int32_t nwritten_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (iovs_len < 0)
        return __WASI_ERRNO_INVAL;

    wasi_iovec_t *iovs =
        vaddr(exec_env, iovs_app, (uint32_t)iovs_len * sizeof(wasi_iovec_t));
    __wasi_size_t *nwritten =
        vaddr(exec_env, nwritten_app, sizeof(__wasi_size_t));
    if (!nwritten)
        return __WASI_ERRNO_FAULT;
    if (!iovs && iovs_len > 0)
        return __WASI_ERRNO_FAULT;

    ssize_t res = 0;
    for (int32_t i = 0; i < iovs_len; i++) {
        uint32_t off = iovs[i].buf;
        uint32_t len = iovs[i].buf_len;
        if (len == 0)
            continue;
        void *addr = vaddr(exec_env, off, len);
        if (!addr)
            return __WASI_ERRNO_FAULT;

        int ret = VfsWrite(ctx->vfsCtx, fd, addr, len);
        if (ret < 0)
            return errno_to_wasi(ret);
        res += ret;
        if ((uint32_t)ret < len)
            break;
    }
    *nwritten = (__wasi_size_t)res;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_readdir(wasm_exec_env_t exec_env,
                               int32_t fd, int32_t buf_app,
                               int32_t buf_len, int64_t cookie,
                               int32_t bufused_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    void *buf = vaddr(exec_env, buf_app, (uint32_t)buf_len);
    __wasi_size_t *bufused =
        vaddr(exec_env, bufused_app, sizeof(__wasi_size_t));
    if (!buf || !bufused)
        return __WASI_ERRNO_FAULT;

    uint64_t last = (uint64_t)cookie;
    size_t used = 0;
    int ret = VfsReadDir(ctx->vfsCtx, fd, buf, (size_t)buf_len, &last, &used);
    if (ret < 0)
        return errno_to_wasi(ret);

    *bufused = (__wasi_size_t)used;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_close(wasm_exec_env_t exec_env, int32_t fd) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    int ret = VfsClose(ctx->vfsCtx, fd);
    return ret < 0 ? errno_to_wasi(ret) : __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_datasync(wasm_exec_env_t exec_env, int32_t fd) {
    (void)exec_env; (void)fd;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_random_get(wasm_exec_env_t exec_env,
                               int32_t buf_app, int32_t buf_len) {
    uint8_t *buf = vaddr(exec_env, buf_app, (uint32_t)buf_len);
    if (!buf)
        return __WASI_ERRNO_FAULT;

    uint32_t remaining = (uint32_t)buf_len;
    while (remaining > 0) {
        int64_t got = PlatfromGetRandom(buf, remaining);
        if (got < 0)
            return errno_to_wasi((int)got);
        buf += got;
        remaining -= (uint32_t)got;
    }
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_clock_res_get(wasm_exec_env_t exec_env,
                                  int32_t wasi_clk_id, int32_t resolution_app) {
    __wasi_timestamp_t *resolution =
        vaddr(exec_env, resolution_app, sizeof(__wasi_timestamp_t));
    if (!resolution)
        return __WASI_ERRNO_FAULT;

    uint64_t res;
    int ret = PlatformClockGetRes(wasi_clk_id, &res);
    if (ret < 0)
        return errno_to_wasi(ret);

    *resolution = res;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_clock_time_get(wasm_exec_env_t exec_env,
                                   int32_t wasi_clk_id, int64_t precision,
                                   int32_t time_app) {
    (void)precision;
    __wasi_timestamp_t *time =
        vaddr(exec_env, time_app, sizeof(__wasi_timestamp_t));
    if (!time)
        return __WASI_ERRNO_FAULT;

    plat_timestamp_t t;
    int ret = PlatformClockGetTime(wasi_clk_id, &t);
    if (ret < 0)
        return errno_to_wasi(ret);

    *time = t;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_poll_oneoff(wasm_exec_env_t exec_env,
                                int32_t in_app, int32_t out_app,
                                int32_t nsubscriptions, int32_t nevents_app) {
    const __wasi_subscription_t *in =
        vaddr(exec_env, in_app,
              (uint32_t)nsubscriptions * sizeof(__wasi_subscription_t));
    uint8_t *out =
        vaddr(exec_env, out_app,
              (uint32_t)nsubscriptions * sizeof(__wasi_event_t));
    __wasi_size_t *nevents =
        vaddr(exec_env, nevents_app, sizeof(__wasi_size_t));
    if (!in || !out || !nevents)
        return __WASI_ERRNO_FAULT;

    /* Only clock subscriptions are supported. fd_read / fd_write
     * subscriptions return NOSYS — wapps that need them must poll the fd
     * directly until full readiness support lands. */
    if (in->type != __WASI_EVENTTYPE_CLOCK)
        return __WASI_ERRNO_NOSYS;

    int ret = PlatformClockNanoSleep(in->u.clock.id, in->u.clock.timeout,
                                     in->u.clock.flags);
    if (ret < 0)
        return errno_to_wasi(ret);

    /* Synthesise a single clock event so the caller's tick loop unblocks.
     * The __wasi_event_t layout (snapshot-preview1):
     *   offset  0: userdata        (u64)
     *   offset  8: error           (u16)
     *   offset 10: type            (u8)   — must mirror the subscription type
     *   offset 11: _pad[5]
     *   offset 16: fd_readwrite    (u64 nbytes + u16 flags) — unused for clock
     * Zero-initialise first, then fill the fields the wapp inspects. */
    memset(out, 0, sizeof(__wasi_event_t));
    *(uint64_t *)(out + 0) = in->userdata;
    *(uint16_t *)(out + 8) = __WASI_ERRNO_SUCCESS;
    *(uint8_t  *)(out + 10) = __WASI_EVENTTYPE_CLOCK;
    *nevents = 1;
    return __WASI_ERRNO_SUCCESS;
}

static void wasi_proc_exit(wasm_exec_env_t exec_env, int32_t code) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (ctx)
        ctx->exit_code = code;
    wasm_runtime_set_exception(inst, "proc_exit");
}

static int32_t wasi_sock_accept(wasm_exec_env_t exec_env,
                                int32_t fd, int32_t flags, int32_t fd_new_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    __wasi_fd_t *fd_new = vaddr(exec_env, fd_new_app, sizeof(__wasi_fd_t));
    if (!fd_new)
        return __WASI_ERRNO_FAULT;

    int f;
    int ret = VfsSockAccept(ctx->vfsCtx, fd, (vfs_oflags_t)flags, &f);
    if (ret < 0)
        return errno_to_wasi(ret);

    *fd_new = (__wasi_fd_t)f;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_sock_recv(wasm_exec_env_t exec_env,
                              int32_t fd, int32_t ri_data_app,
                              int32_t ri_data_len, int32_t ri_flags,
                              int32_t size_app, int32_t ro_flags_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (ri_data_len < 0)
        return __WASI_ERRNO_INVAL;

    wasi_iovec_t *iovs =
        vaddr(exec_env, ri_data_app,
              (uint32_t)ri_data_len * sizeof(wasi_iovec_t));
    __wasi_size_t *size = vaddr(exec_env, size_app, sizeof(__wasi_size_t));
    __wasi_roflags_t *ro_flags =
        vaddr(exec_env, ro_flags_app, sizeof(__wasi_roflags_t));
    if (!size || !ro_flags)
        return __WASI_ERRNO_FAULT;
    if (!iovs && ri_data_len > 0)
        return __WASI_ERRNO_FAULT;

    ssize_t res = 0;
    vfs_riflags_t i_flags = (vfs_riflags_t)ri_flags;
    vfs_roflags_t o_flags = 0;
    for (int32_t i = 0; i < ri_data_len; i++) {
        uint32_t off = iovs[i].buf;
        uint32_t len = iovs[i].buf_len;
        if (len == 0)
            continue;
        void *addr = vaddr(exec_env, off, len);
        if (!addr)
            return __WASI_ERRNO_FAULT;

        int ret = VfsSockRecv(ctx->vfsCtx, fd, addr, len, i_flags, &o_flags);
        if (ret < 0)
            return errno_to_wasi(ret);
        res += ret;
        if ((uint32_t)ret < len)
            break;
    }
    *size = (__wasi_size_t)res;
    *ro_flags = o_flags;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_sock_send(wasm_exec_env_t exec_env,
                              int32_t fd, int32_t si_data_app,
                              int32_t si_data_len, int32_t si_flags,
                              int32_t size_app) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (si_data_len < 0)
        return __WASI_ERRNO_INVAL;

    wasi_iovec_t *iovs =
        vaddr(exec_env, si_data_app,
              (uint32_t)si_data_len * sizeof(wasi_iovec_t));
    __wasi_size_t *size = vaddr(exec_env, size_app, sizeof(__wasi_size_t));
    if (!size)
        return __WASI_ERRNO_FAULT;
    if (!iovs && si_data_len > 0)
        return __WASI_ERRNO_FAULT;

    vfs_siflags_t i_flags = (vfs_siflags_t)si_flags;
    ssize_t res = 0;
    for (int32_t i = 0; i < si_data_len; i++) {
        uint32_t off = iovs[i].buf;
        uint32_t len = iovs[i].buf_len;
        if (len == 0)
            continue;
        void *addr = vaddr(exec_env, off, len);
        if (!addr)
            return __WASI_ERRNO_FAULT;

        int ret = VfsSockSend(ctx->vfsCtx, fd, addr, len, i_flags);
        if (ret < 0)
            return errno_to_wasi(ret);
        res += ret;
        if ((uint32_t)ret < len)
            break;
    }
    *size = (__wasi_size_t)res;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_sock_shutdown(wasm_exec_env_t exec_env,
                                  int32_t fd, int32_t how) {
    wasi_ctx_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    int ret = VfsSockShutdown(ctx->vfsCtx, fd, (vfs_sdflags_t)how);
    if (ret < 0)
        return errno_to_wasi(ret);
    return __WASI_ERRNO_SUCCESS;
}

/* ---- Registration ---- */

/* fd_seek differs between unstable (CUR/END/SET) and preview1 (SET/CUR/END).
 * All other symbols are identical between the two namespaces. */
#define SHARED_NATIVES                                                         \
    { "args_get",            wasi_args_get,            "(ii)i",    NULL },     \
    { "args_sizes_get",      wasi_args_sizes_get,      "(ii)i",    NULL },     \
    { "environ_get",         wasi_environ_get,         "(ii)i",    NULL },     \
    { "environ_sizes_get",   wasi_environ_sizes_get,   "(ii)i",    NULL },     \
    { "fd_prestat_get",      wasi_fd_prestat_get,      "(ii)i",    NULL },     \
    { "fd_prestat_dir_name", wasi_fd_prestat_dir_name, "(iii)i",   NULL },     \
    { "fd_fdstat_get",       wasi_fd_fdstat_get,       "(ii)i",    NULL },     \
    { "fd_fdstat_set_flags", wasi_fd_fdstat_set_flags, "(ii)i",    NULL },     \
    { "fd_filestat_get",     wasi_fd_filestat_get,     "(ii)i",    NULL },     \
    { "path_filestat_get",   wasi_path_filestat_get,   "(iiiii)i", NULL },     \
    { "path_open",           wasi_path_open,           "(iiiiiIIii)i", NULL }, \
    { "path_unlink_file",    wasi_path_unlink_file,    "(iii)i",   NULL },     \
    { "path_rename",         wasi_path_rename,         "(iiiiii)i", NULL },    \
    { "path_create_directory", wasi_path_create_directory, "(iii)i", NULL },   \
    { "fd_read",             wasi_fd_read,             "(iiii)i",  NULL },     \
    { "fd_write",            wasi_fd_write,            "(iiii)i",  NULL },     \
    { "fd_readdir",          wasi_fd_readdir,          "(iiiIi)i", NULL },     \
    { "fd_close",            wasi_fd_close,            "(i)i",     NULL },     \
    { "fd_datasync",         wasi_fd_datasync,         "(i)i",     NULL },     \
    { "random_get",          wasi_random_get,          "(ii)i",    NULL },     \
    { "clock_res_get",       wasi_clock_res_get,       "(ii)i",    NULL },     \
    { "clock_time_get",      wasi_clock_time_get,      "(iIi)i",   NULL },     \
    { "poll_oneoff",         wasi_poll_oneoff,         "(iiii)i",  NULL },     \
    { "proc_exit",           wasi_proc_exit,           "(i)",      NULL },     \
    { "sock_accept",         wasi_sock_accept,         "(iii)i",   NULL },     \
    { "sock_recv",           wasi_sock_recv,           "(iiiiii)i", NULL },    \
    { "sock_send",           wasi_sock_send,           "(iiiii)i", NULL },     \
    { "sock_shutdown",       wasi_sock_shutdown,       "(ii)i",    NULL }

static NativeSymbol wasi_unstable_natives[] = {
    { "fd_seek", wasi_unstable_fd_seek, "(iIii)i", NULL },
    SHARED_NATIVES,
};

static NativeSymbol wasi_preview1_natives[] = {
    { "fd_seek", wasi_preview1_fd_seek, "(iIii)i", NULL },
    SHARED_NATIVES,
};

wasi_ctx_t *InitWasiContext(void) {
    wasi_ctx_t *ctx =
        (wasi_ctx_t *)WantedMalloc(sizeof(wasi_ctx_t));
    if (!ctx)
        return ctx;
    memset(ctx, 0, sizeof(*ctx));

    /* The stdio slots and the root preopen mirror the layout the wapp will see
     * via fd_prestat enumeration. stdio fds 0-2 are registered separately as
     * STREAM slots by VfsRegister — we record them here only so the table is
     * dense for the resolve_preopen scan. The root preopen at fd=3 is lazy:
     * the first prestat_get triggers VfsOpen("/") which will succeed once the
     * wapp setup has called VfsAttachTarfs. */
    static const struct { const char *path; int fd; } seed[] = {
        {"<stdin>",  0},
        {"<stdout>", 1},
        {"<stderr>", 2},
        {"/",       -1},
    };
    for (size_t i = 0; i < sizeof(seed) / sizeof(seed[0]); i++) {
        wasi_preopen_t *p = &ctx->preopens[ctx->preopens_cnt++];
        size_t plen = strlen(seed[i].path);
        if (plen >= sizeof(p->path))
            plen = sizeof(p->path) - 1;
        memcpy(p->path, seed[i].path, plen);
        p->path[plen] = '\0';
        p->fd = seed[i].fd;
    }
    return ctx;
}

void FreeWasiContext(wasi_ctx_t *c) {
    if (!c)
        return;
    /* argv/envp are heap-allocated pointer arrays; their string storage lives
     * in the wapp's persistent launch config (not owned here), so only the
     * arrays are freed. */
    WantedFree((void *)c->argv);
    WantedFree((void *)c->envp);
    WantedFree(c);
}

int WasiCtxAddPreopen(wasi_ctx_t *ctx, const char *path, const char *hostPath,
                      int host_fd, bool readonly) {
    if (!ctx || !path || !ctx->vfsCtx)
        return -EINVAL;
    if (ctx->preopens_cnt >= WASI_MAX_PREOPENS)
        return -ENOSPC;

    /* The PlatformFs driver owns conversion from VFS ops to host syscalls on
     * `host_fd`. The rootPath isn't load-bearing — for openat-relative paths
     * the host kernel resolves against `host_fd` itself — but labelling it with
     * the real backing directory keeps the driver self-describing for
     * debugging. */
    vfs_driver_t *drv =
        VfsPlatformFsInit(NULL, hostPath ? hostPath : path, readonly);
    if (!drv)
        return -ENOMEM;

    int fd = VfsBindPlatformFd(ctx->vfsCtx, path, drv, host_fd, readonly);
    if (fd < 0) {
        if (drv->Destroy)
            drv->Destroy(drv);
        return fd;
    }

    wasi_preopen_t *p = &ctx->preopens[ctx->preopens_cnt++];
    size_t plen = strlen(path);
    if (plen >= sizeof(p->path))
        plen = sizeof(p->path) - 1;
    memcpy(p->path, path, plen);
    p->path[plen] = '\0';
    p->fd = fd;
    return 0;
}

void RegisterWASINatives(void) {
    wasm_runtime_register_natives(
        "wasi_unstable", wasi_unstable_natives,
        sizeof(wasi_unstable_natives) / sizeof(wasi_unstable_natives[0]));
    wasm_runtime_register_natives(
        "wasi_snapshot_preview1", wasi_preview1_natives,
        sizeof(wasi_preview1_natives) / sizeof(wasi_preview1_natives[0]));
}
