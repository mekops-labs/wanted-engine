#include <errno.h>
#include <string.h>

#include <wasm_export.h>

/* wanted includes */
#include <platform.h>
#include <vfs.h>
#include <wanted_malloc.h>
#include <wasi.h>

#include "wasi_types.h"

typedef struct wasi_iovec_t {
    uint32_t buf;     /* wasm linear-memory offset */
    uint32_t buf_len;
} wasi_iovec_t;

typedef struct Preopen {
    const char *path;
} Preopen;

static Preopen preopen[] = {
    {"<stdin>"},
    {"<stdout>"},
    {"<stderr>"},
    {"/"},
};

static const size_t preopen_cnt = sizeof(preopen) / sizeof(preopen[0]);

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

static inline m3_wasi_context_t *get_ctx(wasm_exec_env_t e) {
    return (m3_wasi_context_t *)wasm_runtime_get_user_data(e);
}

/*
 * WASI API implementation
 */

static int32_t wasi_args_get(wasm_exec_env_t exec_env,
                             int32_t argv_app, int32_t argv_buf_app) {
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    (void)exec_env; (void)env_app; (void)env_buf_app;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_environ_sizes_get(wasm_exec_env_t exec_env,
                                      int32_t env_count_app,
                                      int32_t env_buf_size_app) {
    __wasi_size_t *env_count =
        vaddr(exec_env, env_count_app, sizeof(__wasi_size_t));
    __wasi_size_t *env_buf_size =
        vaddr(exec_env, env_buf_size_app, sizeof(__wasi_size_t));
    if (!env_count || !env_buf_size)
        return __WASI_ERRNO_FAULT;
    *env_count = 0;
    *env_buf_size = 0;
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_prestat_dir_name(wasm_exec_env_t exec_env,
                                        int32_t fd, int32_t path_app,
                                        int32_t path_len) {
    if (fd < 3 || (size_t)fd >= preopen_cnt)
        return __WASI_ERRNO_BADF;

    char *path = vaddr(exec_env, path_app, (uint32_t)path_len);
    if (!path)
        return __WASI_ERRNO_FAULT;

    size_t slen = strlen(preopen[fd].path) + 1;
    memcpy(path, preopen[fd].path, slen < (size_t)path_len ? slen : (size_t)path_len);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_prestat_get(wasm_exec_env_t exec_env,
                                   int32_t fd, int32_t buf_app) {
    m3_wasi_context_t *ctx = get_ctx(exec_env);
    if (!ctx)
        return __WASI_ERRNO_INVAL;

    if (fd < 3 || (size_t)fd >= preopen_cnt)
        return __WASI_ERRNO_BADF;

    uint8_t *buf = vaddr(exec_env, buf_app, 8);
    if (!buf)
        return __WASI_ERRNO_FAULT;

    int host_fd = VfsOpen(ctx->vfsCtx, preopen[fd].path,
                          VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd != host_fd) {
        VfsClose(ctx->vfsCtx, host_fd);
        return __WASI_ERRNO_BADF;
    }

    *(uint32_t *)(buf + 0) = __WASI_PREOPENTYPE_DIR;
    *(uint32_t *)(buf + 4) = (uint32_t)(strlen(preopen[fd].path) + 1);
    return __WASI_ERRNO_SUCCESS;
}

static int32_t wasi_fd_fdstat_get(wasm_exec_env_t exec_env,
                                  int32_t fd, int32_t fdstat_app) {
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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

static int32_t wasi_fd_read(wasm_exec_env_t exec_env,
                            int32_t fd, int32_t iovs_app,
                            int32_t iovs_len, int32_t nread_app) {
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    void *out =
        vaddr(exec_env, out_app,
              (uint32_t)nsubscriptions * sizeof(__wasi_event_t));
    __wasi_size_t *nevents =
        vaddr(exec_env, nevents_app, sizeof(__wasi_size_t));
    if (!in || !out || !nevents)
        return __WASI_ERRNO_FAULT;

    if (in->type == __WASI_EVENTTYPE_CLOCK) {
        int ret = PlatformClockNanoSleep(in->u.clock.id, in->u.clock.timeout,
                                         in->u.clock.flags);
        if (ret < 0)
            return errno_to_wasi(ret);
    }

    /* TODO: full poll_oneoff implementation */
    return __WASI_ERRNO_NOSYS;
}

static void wasi_proc_exit(wasm_exec_env_t exec_env, int32_t code) {
    wasm_module_inst_t inst = wasm_runtime_get_module_inst(exec_env);
    m3_wasi_context_t *ctx = get_ctx(exec_env);
    if (ctx)
        ctx->exit_code = code;
    wasm_runtime_set_exception(inst, "proc_exit");
}

static int32_t wasi_sock_accept(wasm_exec_env_t exec_env,
                                int32_t fd, int32_t flags, int32_t fd_new_app) {
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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
    m3_wasi_context_t *ctx = get_ctx(exec_env);
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

m3_wasi_context_t *InitWasiContext(void) {
    m3_wasi_context_t *ctx =
        (m3_wasi_context_t *)WantedMalloc(sizeof(m3_wasi_context_t));
    if (ctx)
        memset(ctx, 0, sizeof(*ctx));
    return ctx;
}

void FreeWasiContext(m3_wasi_context_t *c) { WantedFree(c); }

void RegisterWASINatives(void) {
    wasm_runtime_register_natives(
        "wasi_unstable", wasi_unstable_natives,
        sizeof(wasi_unstable_natives) / sizeof(wasi_unstable_natives[0]));
    wasm_runtime_register_natives(
        "wasi_snapshot_preview1", wasi_preview1_natives,
        sizeof(wasi_preview1_natives) / sizeof(wasi_preview1_natives[0]));
}
