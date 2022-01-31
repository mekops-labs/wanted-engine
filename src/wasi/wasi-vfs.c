#include <sys/types.h>
#include <fcntl.h>
#include <sys/random.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/mman.h>

#include "m3_env.h"
#include "m3_exception.h"

#include "extra/wasi_core.h"

#include "wasi.h"
#include "romfs.h"
#include "vfs.h"

extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;
extern vfs_driver_t vfs_virtual_drv;

static m3_wasi_context_t wasi_context;

typedef struct wasi_iovec_t
{
    __wasi_size_t buf;
    __wasi_size_t buf_len;
} wasi_iovec_t;

typedef struct Preopen {
    int             fd;
    const char*     path;
    const char*     hpath;
    vfs_driver_t    *drv;
    bool            opened;
} Preopen;

#define MAX_OPEN 20
Preopen preopen[MAX_OPEN] = {
    {  0, "<stdin>" , "", &vfs_linux_drv, true },
    {  1, "<stdout>", "", &vfs_linux_drv, true },
    {  2, "<stderr>", "", &vfs_linux_drv, true },
    { -1, "/"       , "/", &vfs_virtual_drv, true },
    { -1, "/dir"    , ".", &vfs_linux_drv, true },
    { -1, "/rom"    , "/", &vfs_romfs_drv, true },
};

static
int FindFirstClosedFd()
{
    for (int i = 0; i < MAX_OPEN; i++) {
        if (!preopen[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}

#  define CASE_RET(e1,e2)     case e1:   return e2;   break

static
__wasi_errno_t errno_to_wasi(int errnum) {
    switch (errnum) {
    CASE_RET( EPERM       , __WASI_ERRNO_PERM         );
    CASE_RET( ENOENT      , __WASI_ERRNO_NOENT        );
    CASE_RET( ESRCH       , __WASI_ERRNO_SRCH         );
    CASE_RET( EINTR       , __WASI_ERRNO_INTR         );
    CASE_RET( EIO         , __WASI_ERRNO_IO           );
    CASE_RET( ENXIO       , __WASI_ERRNO_NXIO         );
    CASE_RET( E2BIG       , __WASI_ERRNO_2BIG         );
    CASE_RET( ENOEXEC     , __WASI_ERRNO_NOEXEC       );
    CASE_RET( EBADF       , __WASI_ERRNO_BADF         );
    CASE_RET( ECHILD      , __WASI_ERRNO_CHILD        );
    CASE_RET( EAGAIN      , __WASI_ERRNO_AGAIN        );
    CASE_RET( ENOMEM      , __WASI_ERRNO_NOMEM        );
    CASE_RET( EACCES      , __WASI_ERRNO_ACCES        );
    CASE_RET( EFAULT      , __WASI_ERRNO_FAULT        );
    CASE_RET( EBUSY       , __WASI_ERRNO_BUSY         );
    CASE_RET( EEXIST      , __WASI_ERRNO_EXIST        );
    CASE_RET( EXDEV       , __WASI_ERRNO_XDEV         );
    CASE_RET( ENODEV      , __WASI_ERRNO_NODEV        );
    CASE_RET( ENOTDIR     , __WASI_ERRNO_NOTDIR       );
    CASE_RET( EISDIR      , __WASI_ERRNO_ISDIR        );
    CASE_RET( EINVAL      , __WASI_ERRNO_INVAL        );
    CASE_RET( ENFILE      , __WASI_ERRNO_NFILE        );
    CASE_RET( EMFILE      , __WASI_ERRNO_MFILE        );
    CASE_RET( ENOTTY      , __WASI_ERRNO_NOTTY        );
    CASE_RET( ETXTBSY     , __WASI_ERRNO_TXTBSY       );
    CASE_RET( EFBIG       , __WASI_ERRNO_FBIG         );
    CASE_RET( ENOSPC      , __WASI_ERRNO_NOSPC        );
    CASE_RET( ESPIPE      , __WASI_ERRNO_SPIPE        );
    CASE_RET( EROFS       , __WASI_ERRNO_ROFS         );
    CASE_RET( EMLINK      , __WASI_ERRNO_MLINK        );
    CASE_RET( EPIPE       , __WASI_ERRNO_PIPE         );
    CASE_RET( EDOM        , __WASI_ERRNO_DOM          );
    CASE_RET( ERANGE      , __WASI_ERRNO_RANGE        );
    CASE_RET( ENAMETOOLONG, __WASI_ERRNO_NAMETOOLONG  );
    }
    return __WASI_ERRNO_INVAL;
}

static inline
int convert_clockid(__wasi_clockid_t in) {
    switch (in) {
    case __WASI_CLOCKID_MONOTONIC:            return CLOCK_MONOTONIC;
    //case __WASI_CLOCKID_PROCESS_CPUTIME_ID:   return CLOCK_PROCESS_CPUTIME_ID;
    case __WASI_CLOCKID_REALTIME:             return CLOCK_REALTIME;
    //case __WASI_CLOCKID_THREAD_CPUTIME_ID:    return CLOCK_THREAD_CPUTIME_ID;
    default: return -1;
    }
}

static inline
__wasi_timestamp_t convert_timespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((__wasi_timestamp_t)ts->tv_sec >= UINT64_MAX / 1000000000)
        return UINT64_MAX;
    return (__wasi_timestamp_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

static inline
struct timespec convert_timestamp(__wasi_timestamp_t t) {
    struct timespec ts;

    ts.tv_sec = t / 1000000000ULL;
    ts.tv_nsec = t % 1000000000LL;

    return ts;
}

/*
 * WASI API implementation
 */

m3ApiRawFunction(m3_wasi_generic_args_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *           , argv)
    m3ApiGetArgMem   (char *               , argv_buf)

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context == NULL) { m3ApiReturn(__WASI_ERRNO_INVAL); }

    m3ApiCheckMem(argv, context->argc * sizeof(uint32_t));

    for (u32 i = 0; i < context->argc; ++i)
    {
        m3ApiWriteMem32(&argv[i], m3ApiPtrToOffset(argv_buf));

        size_t len = strlen (context->argv[i]);

        m3ApiCheckMem(argv_buf, len);
        memcpy (argv_buf, context->argv[i], len);
        argv_buf += len;
        * argv_buf++ = 0;
    }

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_args_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t *      , argc)
    m3ApiGetArgMem   (__wasi_size_t *      , argv_buf_size)

    m3ApiCheckMem(argc,             sizeof(__wasi_size_t));
    m3ApiCheckMem(argv_buf_size,    sizeof(__wasi_size_t));

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context == NULL) { m3ApiReturn(__WASI_ERRNO_INVAL); }

    __wasi_size_t buf_len = 0;
    for (u32 i = 0; i < context->argc; ++i)
    {
        buf_len += strlen (context->argv[i]) + 1;
    }

    m3ApiWriteMem32(argc, context->argc);
    m3ApiWriteMem32(argv_buf_size, buf_len);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_environ_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t *           , env)
    m3ApiGetArgMem   (char *               , env_buf)

    // TODO
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (__wasi_size_t *      , env_count)
    m3ApiGetArgMem   (__wasi_size_t *      , env_buf_size)

    m3ApiCheckMem(env_count,    sizeof(__wasi_size_t));
    m3ApiCheckMem(env_buf_size, sizeof(__wasi_size_t));

    // TODO
    m3ApiWriteMem32(env_count,    0);
    m3ApiWriteMem32(env_buf_size, 0);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_prestat_dir_name)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (char *               , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)

    m3ApiCheckMem(path, path_len);

    if (fd < 3 || fd >= MAX_OPEN) { m3ApiReturn(__WASI_ERRNO_BADF); }
    if (preopen[fd].path == NULL) { m3ApiReturn(__WASI_ERRNO_BADF); }

    size_t slen = strlen(preopen[fd].path) + 1;
    memcpy(path, preopen[fd].path, M3_MIN(slen, path_len));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_prestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (uint8_t *            , buf)

    m3ApiCheckMem(buf, 8);

    if (fd < 3 || fd >= MAX_OPEN) { m3ApiReturn(__WASI_ERRNO_BADF); }
    if (preopen[fd].path == NULL) { m3ApiReturn(__WASI_ERRNO_BADF); }

    if (preopen[fd].fd == -1) {
        preopen[fd].fd = preopen[fd].drv->Open(preopen[fd].hpath, O_DIRECTORY);
    }

    m3ApiWriteMem32(buf+0, __WASI_PREOPENTYPE_DIR);
    m3ApiWriteMem32(buf+4, strlen(preopen[fd].path) + 1);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_fdstat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (__wasi_fdstat_t *    , fdstat)

    m3ApiCheckMem(fdstat, sizeof(__wasi_fdstat_t));

    vfs_fdstat_t stat;

    int ret = preopen[fd].drv->FdStat(preopen[fd].fd, &stat);
    if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));

    fdstat->fs_filetype = stat.filetype;

    m3ApiWriteMem16(&fdstat->fs_flags, 0); // no flags supported
    fdstat->fs_rights_base = (uint64_t)-1; // all rights

    fdstat->fs_rights_inheriting = fdstat->fs_rights_base; // all rights

    // Make descriptors 0,1,2 look like a TTY
    if (fd <= 2) {
        fdstat->fs_rights_base &= ~(__WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL);
    }

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_fdstat_set_flags)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_fdflags_t     , flags)

    // TODO

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_unstable_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filedelta_t   , offset)
    m3ApiGetArg      (uint32_t             , wasi_whence)
    m3ApiGetArgMem   (__wasi_filesize_t *  , result)

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));

    int whence;

    switch (wasi_whence) {
    case 0: whence = ROMFS_SEEK_CUR; break;
    case 1: whence = ROMFS_SEEK_END; break;
    case 2: whence = ROMFS_SEEK_SET; break;
    default:                m3ApiReturn(__WASI_ERRNO_INVAL);
    }

    long pos;
    int ret = preopen[fd].drv->Seek(preopen[fd].fd, offset, whence, &pos);
    if (ret < 0) { m3ApiReturn(errno_to_wasi(-ret)); }
    m3ApiWriteMem64(result, pos);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_snapshot_preview1_fd_seek)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_filedelta_t   , offset)
    m3ApiGetArg      (uint32_t             , wasi_whence)
    m3ApiGetArgMem   (__wasi_filesize_t *  , result)

    m3ApiCheckMem(result, sizeof(__wasi_filesize_t));

    int whence;

    switch (wasi_whence) {
    case 0: whence = ROMFS_SEEK_SET; break;
    case 1: whence = ROMFS_SEEK_CUR; break;
    case 2: whence = ROMFS_SEEK_END; break;
    default:                m3ApiReturn(__WASI_ERRNO_INVAL);
    }

    long pos;
    int ret = preopen[fd].drv->Seek(preopen[fd].fd, offset, whence, &pos);
    if (ret < 0) { m3ApiReturn(errno_to_wasi(-ret)); }
    m3ApiWriteMem64(result, pos);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_snapshot_preview1_path_filestat_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArg      (__wasi_lookupflags_t , flags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (uint32_t             , path_len)
    m3ApiGetArgMem   (uint8_t *            , buf)

    if (path_len >= 512)
        m3ApiReturn(__WASI_ERRNO_INVAL);

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf,  64); // wasi_filestat_t

    // copy path so we can ensure it is NULL terminated
    char host_path[path_len+1];
    memcpy (host_path, path, path_len);
    host_path[path_len] = '\0'; // NULL terminator

    vfs_filestat_t statbuf;
    __wasi_filestat_t stat;

    int ret = preopen[fd].drv->FileStatAt(preopen[fd].fd, host_path, &statbuf);
    if (ret < 0) { m3ApiReturn(errno_to_wasi(-ret)); }

    stat.filetype = statbuf.filetype;
    stat.dev      = statbuf.dev;
    stat.ino      = statbuf.ino;
    stat.nlink    = statbuf.nlink;
    stat.size     = statbuf.size;
    stat.atim     = statbuf.atim;
    stat.mtim     = statbuf.mtim;
    stat.ctim     = statbuf.ctim;

    memset(buf, 0, 64);
    m3ApiWriteMem64(buf+0,  stat.dev);
    m3ApiWriteMem64(buf+8,  stat.ino);
    m3ApiWriteMem8 (buf+16, stat.filetype);
    m3ApiWriteMem64(buf+24, stat.nlink);
    m3ApiWriteMem64(buf+32, stat.size);
    m3ApiWriteMem64(buf+40, stat.atim);
    m3ApiWriteMem64(buf+48, stat.mtim);
    m3ApiWriteMem64(buf+56, stat.ctim);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_path_open)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , dirfd)
    m3ApiGetArg      (__wasi_lookupflags_t , dirflags)
    m3ApiGetArgMem   (const char *         , path)
    m3ApiGetArg      (__wasi_size_t        , path_len)
    m3ApiGetArg      (__wasi_oflags_t      , oflags)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_base)
    m3ApiGetArg      (__wasi_rights_t      , fs_rights_inheriting)
    m3ApiGetArg      (__wasi_fdflags_t     , fs_flags)
    m3ApiGetArgMem   (__wasi_fd_t *        , fd)

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(fd,   sizeof(__wasi_fd_t));

    if (path_len >= 512)
        m3ApiReturn(__WASI_ERRNO_INVAL);

    int host_fd;

    // copy path so we can ensure it is NULL terminated
    char host_path[path_len+1];
    memcpy (host_path, path, path_len);
    host_path[path_len] = '\0'; // NULL terminator

    // translate o_flags and fs_flags into flags and mode
    int flags = ((oflags & __WASI_OFLAGS_CREAT)             ? VFS_O_CREAT     : 0) |
                ((oflags & __WASI_OFLAGS_DIRECTORY)         ? VFS_O_DIRECTORY : 0) |
                ((oflags & __WASI_OFLAGS_EXCL)              ? VFS_O_EXCL      : 0) |
                ((oflags & __WASI_OFLAGS_TRUNC)             ? VFS_O_TRUNC     : 0) |
                ((fs_flags & __WASI_FDFLAGS_APPEND)         ? VFS_O_APPEND    : 0) |
                ((fs_flags & __WASI_FDFLAGS_DSYNC)          ? VFS_O_DSYNC     : 0) |
                ((fs_flags & __WASI_FDFLAGS_NONBLOCK)       ? VFS_O_NONBLOCK  : 0) |
                ((fs_flags & __WASI_FDFLAGS_RSYNC)          ? VFS_O_RSYNC     : 0) |
                ((fs_flags & __WASI_FDFLAGS_SYNC)           ? VFS_O_SYNC      : 0);

    if ((fs_rights_base & __WASI_RIGHTS_FD_READ) &&
        (fs_rights_base & __WASI_RIGHTS_FD_WRITE)) {
        flags |= VFS_O_RDWR;
    } else if ((fs_rights_base & __WASI_RIGHTS_FD_WRITE)) {
        flags |= VFS_O_WRONLY;
    } else if ((fs_rights_base & __WASI_RIGHTS_FD_READ)) {
        flags |= VFS_O_RDONLY; // no-op because O_RDONLY is 0
    }

    host_fd = preopen[dirfd].drv->OpenAt(preopen[dirfd].fd, host_path, flags);
    if (host_fd < 0) {
        m3ApiReturn(errno_to_wasi (-host_fd));
    } else {
        int ret = FindFirstClosedFd();
        if (ret < 0) {
            preopen[dirfd].drv->Close(host_fd);
            m3ApiReturn(errno_to_wasi (-ret));
        }
        preopen[ret].fd = host_fd;
        preopen[ret].opened = true;
        preopen[ret].drv = preopen[dirfd].drv;

        m3ApiWriteMem32(fd, ret);
        m3ApiReturn(__WASI_ERRNO_SUCCESS);
    }
}

m3ApiRawFunction(m3_wasi_generic_fd_read)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t *      , nread)

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nread,        sizeof(__wasi_size_t));

    ssize_t res = 0;
    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        void* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;

        int ret = preopen[fd].drv->Read(preopen[fd].fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        if ((size_t)ret < len) break;
    }
    m3ApiWriteMem32(nread, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_write)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (wasi_iovec_t *       , wasi_iovs)
    m3ApiGetArg      (__wasi_size_t        , iovs_len)
    m3ApiGetArgMem   (__wasi_size_t *      , nwritten)

    m3ApiCheckMem(wasi_iovs,    iovs_len * sizeof(wasi_iovec_t));
    m3ApiCheckMem(nwritten,     sizeof(__wasi_size_t));

    if (fd > 2) {
        m3ApiReturn(__WASI_ERRNO_ROFS)
    }

    ssize_t res = 0;
    for (__wasi_size_t i = 0; i < iovs_len; i++) {
        void* addr = m3ApiOffsetToPtr(m3ApiReadMem32(&wasi_iovs[i].buf));
        size_t len = m3ApiReadMem32(&wasi_iovs[i].buf_len);
        if (len == 0) continue;

        int ret = preopen[fd].drv->Write(preopen[fd].fd, addr, len);
        if (ret < 0) m3ApiReturn(errno_to_wasi(-ret));
        res += ret;
        if ((size_t)ret < len) break;
    }
    m3ApiWriteMem32(nwritten, res);
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_readdir)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t          , fd)
    m3ApiGetArgMem   (void *               , buf)
    m3ApiGetArg      (__wasi_size_t        , buf_len)
    m3ApiGetArg      (__wasi_dircookie_t   , cookie)
    m3ApiGetArgMem   (__wasi_size_t *      , bufused)

    m3ApiCheckMem(buf,      buf_len);
    m3ApiCheckMem(bufused,  sizeof(__wasi_size_t));

    int ret;
    uint64_t last = cookie;
    size_t used = 0;

    ret = preopen[fd].drv->ReadDir(preopen[fd].fd, buf, buf_len, &last, &used);
    if (ret < 0) {
        m3ApiReturn(errno_to_wasi(-ret));
    }

    m3ApiWriteMem32(bufused, (__wasi_size_t)used);

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_close)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    if (fd >= MAX_OPEN) m3ApiReturn(errno_to_wasi(-EBADF));
    int ret = preopen[fd].drv->Close(fd);
    preopen[fd].opened = false;

    m3ApiReturn(ret < 0 ? errno_to_wasi(-ret) : __WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_fd_datasync)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_fd_t, fd)

    // TODO
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_random_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint8_t *            , buf)
    m3ApiGetArg      (__wasi_size_t        , buf_len)

    m3ApiCheckMem(buf, buf_len);

    while (1) {
        ssize_t retlen = 0;

        retlen = getrandom(buf, buf_len, 0);

        if (retlen < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            m3ApiReturn(errno_to_wasi(errno));
        } else if (retlen == buf_len) {
            m3ApiReturn(__WASI_ERRNO_SUCCESS);
        } else {
            buf     += retlen;
            buf_len -= retlen;
        }
    }
}

m3ApiRawFunction(m3_wasi_generic_clock_res_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArgMem   (__wasi_timestamp_t * , resolution)

    m3ApiCheckMem(resolution, sizeof(__wasi_timestamp_t));

    int clk = convert_clockid(wasi_clk_id);
    if (clk < 0) m3ApiReturn(__WASI_ERRNO_INVAL);

    struct timespec tp;
    if (clock_getres(clk, &tp) != 0) {
        m3ApiWriteMem64(resolution, 1000000);
    } else {
        m3ApiWriteMem64(resolution, convert_timespec(&tp));
    }

    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_clock_time_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (__wasi_clockid_t     , wasi_clk_id)
    m3ApiGetArg      (__wasi_timestamp_t   , precision)
    m3ApiGetArgMem   (__wasi_timestamp_t * , time)

    m3ApiCheckMem(time, sizeof(__wasi_timestamp_t));

    int clk = convert_clockid(wasi_clk_id);
    if (clk < 0) m3ApiReturn(__WASI_ERRNO_INVAL);

    struct timespec tp;
    if (clock_gettime(clk, &tp) != 0) {
        m3ApiReturn(errno_to_wasi(errno));
    }

    m3ApiWriteMem64(time, convert_timespec(&tp));
    m3ApiReturn(__WASI_ERRNO_SUCCESS);
}

m3ApiRawFunction(m3_wasi_generic_poll_oneoff)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (const __wasi_subscription_t * , in)
    m3ApiGetArgMem   (__wasi_event_t *              , out)
    m3ApiGetArg      (__wasi_size_t                 , nsubscriptions)
    m3ApiGetArgMem   (__wasi_size_t *               , nevents)

    m3ApiCheckMem(in,       nsubscriptions * sizeof(__wasi_subscription_t));
    m3ApiCheckMem(out,      nsubscriptions * sizeof(__wasi_event_t));
    m3ApiCheckMem(nevents,  sizeof(__wasi_size_t));

    __wasi_errno_t ret = __WASI_ERRNO_NOSYS;

    if (in->type == __WASI_EVENTTYPE_CLOCK) {
        struct timespec rqtp = convert_timestamp(in->u.clock.timeout);

        clock_nanosleep(
            convert_clockid(in->u.clock.id),
            in->u.clock.flags,
            &rqtp, NULL
            );
    }


    //TODO: m3ApiWriteMem

    m3ApiReturn(ret);
}

m3ApiRawFunction(m3_wasi_generic_proc_exit)
{
    m3ApiGetArg      (uint32_t, code)

    m3_wasi_context_t* context = (m3_wasi_context_t*)(_ctx->userdata);

    if (context) {
        context->exit_code = code;
    }

    m3ApiTrap(m3Err_trapExit);
}


static
M3Result SuppressLookupFailure(M3Result i_result)
{
    if (i_result == m3Err_functionLookupFailed)
        return m3Err_none;
    else
        return i_result;
}

m3_wasi_context_t* GetWasiContext()
{
    return &wasi_context;
}

M3Result  LinkWASI  (IM3Module module)
{
    M3Result result = m3Err_none;

    RomfsLoad(wasi_context.RomfsImg, wasi_context.RomfsImgLen);

    static const char* namespaces[2] = { "wasi_unstable", "wasi_snapshot_preview1" };

    // fd_seek is incompatible
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespaces[0], "fd_seek",     "i(iIi*)", &m3_wasi_unstable_fd_seek)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespaces[1], "fd_seek",     "i(iIi*)", &m3_wasi_snapshot_preview1_fd_seek)));
//_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespaces[0], "path_filestat_get", "i(ii*i*)",  &m3_wasi_unstable_path_filestat_get)));
_   (SuppressLookupFailure (m3_LinkRawFunction (module, namespaces[1], "path_filestat_get", "i(ii*i*)",  &m3_wasi_snapshot_preview1_path_filestat_get)));
    for (int i=0; i<2; i++)
    {
        const char* wasi = namespaces[i];

_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "args_get",           "i(**)",   &m3_wasi_generic_args_get, &wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "args_sizes_get",     "i(**)",   &m3_wasi_generic_args_sizes_get, &wasi_context)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "clock_res_get",        "i(i*)",   &m3_wasi_generic_clock_res_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "clock_time_get",       "i(iI*)",  &m3_wasi_generic_clock_time_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "environ_get",          "i(**)",   &m3_wasi_generic_environ_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "environ_sizes_get",    "i(**)",   &m3_wasi_generic_environ_sizes_get)));

//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_advise",            "i(iIIi)", )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_allocate",          "i(iII)",  )));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_close",             "i(i)",    &m3_wasi_generic_fd_close)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_datasync",          "i(i)",    &m3_wasi_generic_fd_datasync)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_get",        "i(i*)",   &m3_wasi_generic_fd_fdstat_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_set_flags",  "i(ii)",   &m3_wasi_generic_fd_fdstat_set_flags)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_fdstat_set_rights", "i(iII)",  )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_filestat_get",      "i(i*)",   )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_filestat_set_size", "i(iI)",   )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_filestat_set_times","i(iIIi)", )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_pread",             "i(i*iI*)",)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_prestat_get",       "i(i*)",   &m3_wasi_generic_fd_prestat_get)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_prestat_dir_name",  "i(i*i)",  &m3_wasi_generic_fd_prestat_dir_name)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_pwrite",            "i(i*iI*)",)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_read",              "i(i*i*)", &m3_wasi_generic_fd_read)));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_readdir",           "i(i*iI*)", &m3_wasi_generic_fd_readdir)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_renumber",          "i(ii)",   )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_sync",              "i(i)",    )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_tell",              "i(i*)",   )));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "fd_write",             "i(i*i*)", &m3_wasi_generic_fd_write)));

//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_create_directory",    "i(i*i)",       )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_filestat_get",        "i(ii*i*)",     &m3_wasi_generic_path_filestat_get)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_filestat_set_times",  "i(ii*iIIi)",   )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_link",                "i(ii*ii*i)",   )));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_open",                "i(ii*iiIIi*)", &m3_wasi_generic_path_open)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_readlink",            "i(i*i*i*)",    )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_remove_directory",    "i(i*i)",       )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_rename",              "i(i*ii*i)",    )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_symlink",             "i(*ii*i)",     )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "path_unlink_file",         "i(i*i)",       )));

_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "poll_oneoff",          "i(**i*)", &m3_wasi_generic_poll_oneoff)));
_       (SuppressLookupFailure (m3_LinkRawFunctionEx (module, wasi, "proc_exit",          "v(i)",    &m3_wasi_generic_proc_exit, &wasi_context)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "proc_raise",           "i(i)",    )));
_       (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "random_get",           "i(*i)",   &m3_wasi_generic_random_get)));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "sched_yield",          "i()",     )));

//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "sock_recv",            "i(i*ii**)",        )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "sock_send",            "i(i*ii*)",         )));
//_     (SuppressLookupFailure (m3_LinkRawFunction (module, wasi, "sock_shutdown",        "i(ii)",            )));
    }

_catch:
    return result;
}
