/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/*
 * Minimal WASI snapshot-preview1 types — values match wasi-libc spec.
 * Maintained locally because WAMR_BUILD_LIBC_WASI=0 (we have a custom
 * VFS-backed implementation).
 */

#include <stdint.h>

typedef uint16_t __wasi_errno_t;
typedef uint32_t __wasi_fd_t;
typedef uint32_t __wasi_size_t;
typedef int64_t __wasi_filedelta_t;
typedef uint64_t __wasi_filesize_t;
typedef uint64_t __wasi_timestamp_t;
typedef uint64_t __wasi_dircookie_t;
typedef uint64_t __wasi_inode_t;
typedef uint64_t __wasi_device_t;
typedef uint64_t __wasi_linkcount_t;
typedef uint64_t __wasi_userdata_t;
typedef uint64_t __wasi_rights_t;
typedef uint32_t __wasi_lookupflags_t;
typedef uint32_t __wasi_clockid_t;
typedef uint16_t __wasi_oflags_t;
typedef uint16_t __wasi_fdflags_t;
typedef uint16_t __wasi_riflags_t;
typedef uint16_t __wasi_roflags_t;
typedef uint16_t __wasi_siflags_t;
typedef uint16_t __wasi_subclockflags_t;
typedef uint8_t __wasi_filetype_t;
typedef uint8_t __wasi_preopentype_t;
typedef uint8_t __wasi_eventtype_t;
typedef uint8_t __wasi_sdflags_t;

/* errno — values from wasi-libc public/wasi/api.h */
#define __WASI_ERRNO_SUCCESS 0
#define __WASI_ERRNO_2BIG 1
#define __WASI_ERRNO_ACCES 2
#define __WASI_ERRNO_AGAIN 6
#define __WASI_ERRNO_BADF 8
#define __WASI_ERRNO_BUSY 10
#define __WASI_ERRNO_CHILD 12
#define __WASI_ERRNO_CONNABORTED 13
#define __WASI_ERRNO_CONNREFUSED 14
#define __WASI_ERRNO_CONNRESET 15
#define __WASI_ERRNO_DESTADDRREQ 17
#define __WASI_ERRNO_DOM 18
#define __WASI_ERRNO_EXIST 20
#define __WASI_ERRNO_FAULT 21
#define __WASI_ERRNO_FBIG 22
#define __WASI_ERRNO_INTR 27
#define __WASI_ERRNO_INVAL 28
#define __WASI_ERRNO_IO 29
#define __WASI_ERRNO_ISDIR 31
#define __WASI_ERRNO_MFILE 33
#define __WASI_ERRNO_MLINK 34
#define __WASI_ERRNO_NAMETOOLONG 37
#define __WASI_ERRNO_NFILE 41
#define __WASI_ERRNO_NODEV 43
#define __WASI_ERRNO_NOENT 44
#define __WASI_ERRNO_NOEXEC 45
#define __WASI_ERRNO_NOMEM 48
#define __WASI_ERRNO_NOSPC 51
#define __WASI_ERRNO_NOSYS 52
#define __WASI_ERRNO_NOTDIR 54
#define __WASI_ERRNO_NOTTY 59
#define __WASI_ERRNO_NXIO 60
#define __WASI_ERRNO_PERM 63
#define __WASI_ERRNO_PIPE 64
#define __WASI_ERRNO_RANGE 68
#define __WASI_ERRNO_ROFS 69
#define __WASI_ERRNO_SPIPE 70
#define __WASI_ERRNO_SRCH 71
#define __WASI_ERRNO_TXTBSY 74
#define __WASI_ERRNO_XDEV 75
#define __WASI_ERRNO_NOTCAPABLE 76

#define __WASI_OFLAGS_CREAT (1 << 0)
#define __WASI_OFLAGS_DIRECTORY (1 << 1)
#define __WASI_OFLAGS_EXCL (1 << 2)
#define __WASI_OFLAGS_TRUNC (1 << 3)

#define __WASI_FDFLAGS_APPEND (1 << 0)
#define __WASI_FDFLAGS_DSYNC (1 << 1)
#define __WASI_FDFLAGS_NONBLOCK (1 << 2)
#define __WASI_FDFLAGS_RSYNC (1 << 3)
#define __WASI_FDFLAGS_SYNC (1 << 4)

#define __WASI_RIGHTS_FD_DATASYNC (1ULL << 0)
#define __WASI_RIGHTS_FD_READ (1ULL << 1)
#define __WASI_RIGHTS_FD_SEEK (1ULL << 2)
#define __WASI_RIGHTS_FD_FDSTAT_SET_FLAGS (1ULL << 3)
#define __WASI_RIGHTS_FD_SYNC (1ULL << 4)
#define __WASI_RIGHTS_FD_TELL (1ULL << 5)
#define __WASI_RIGHTS_FD_WRITE (1ULL << 6)
#define __WASI_RIGHTS_FD_ADVISE (1ULL << 7)
#define __WASI_RIGHTS_FD_ALLOCATE (1ULL << 8)
#define __WASI_RIGHTS_PATH_CREATE_DIRECTORY (1ULL << 9)
#define __WASI_RIGHTS_PATH_CREATE_FILE (1ULL << 10)
#define __WASI_RIGHTS_PATH_LINK_SOURCE (1ULL << 11)
#define __WASI_RIGHTS_PATH_LINK_TARGET (1ULL << 12)
#define __WASI_RIGHTS_PATH_OPEN (1ULL << 13)
#define __WASI_RIGHTS_FD_READDIR (1ULL << 14)
#define __WASI_RIGHTS_PATH_READLINK (1ULL << 15)
#define __WASI_RIGHTS_PATH_RENAME_SOURCE (1ULL << 16)
#define __WASI_RIGHTS_PATH_RENAME_TARGET (1ULL << 17)
#define __WASI_RIGHTS_PATH_FILESTAT_GET (1ULL << 18)
#define __WASI_RIGHTS_PATH_FILESTAT_SET_SIZE (1ULL << 19)
#define __WASI_RIGHTS_PATH_FILESTAT_SET_TIMES (1ULL << 20)
#define __WASI_RIGHTS_FD_FILESTAT_GET (1ULL << 21)
#define __WASI_RIGHTS_FD_FILESTAT_SET_SIZE (1ULL << 22)
#define __WASI_RIGHTS_FD_FILESTAT_SET_TIMES (1ULL << 23)
#define __WASI_RIGHTS_PATH_SYMLINK (1ULL << 24)
#define __WASI_RIGHTS_PATH_REMOVE_DIRECTORY (1ULL << 25)
#define __WASI_RIGHTS_PATH_UNLINK_FILE (1ULL << 26)
#define __WASI_RIGHTS_POLL_FD_READWRITE (1ULL << 27)
#define __WASI_RIGHTS_SOCK_SHUTDOWN (1ULL << 28)
#define __WASI_RIGHTS_SOCK_ACCEPT (1ULL << 29)

/* Write/mutate class cleared from a read-only grant (READONLY = ALL & ~WRITE).
 * Deny-by-subtraction: any right outside this set still passes, so broadening
 * it only tightens. FD_SYNC and FD_FDSTAT_SET_FLAGS stay out — a durability
 * barrier and an fd-flag toggle, harmless on a read-only fd. */
#define WASI_RIGHTS_WRITE                                                      \
    (__WASI_RIGHTS_FD_DATASYNC | __WASI_RIGHTS_FD_WRITE |                      \
     __WASI_RIGHTS_FD_ALLOCATE | __WASI_RIGHTS_PATH_CREATE_DIRECTORY |         \
     __WASI_RIGHTS_PATH_CREATE_FILE | __WASI_RIGHTS_PATH_LINK_SOURCE |         \
     __WASI_RIGHTS_PATH_LINK_TARGET | __WASI_RIGHTS_PATH_RENAME_SOURCE |       \
     __WASI_RIGHTS_PATH_RENAME_TARGET | __WASI_RIGHTS_PATH_FILESTAT_SET_SIZE | \
     __WASI_RIGHTS_PATH_FILESTAT_SET_TIMES |                                   \
     __WASI_RIGHTS_FD_FILESTAT_SET_SIZE |                                      \
     __WASI_RIGHTS_FD_FILESTAT_SET_TIMES | __WASI_RIGHTS_PATH_SYMLINK |        \
     __WASI_RIGHTS_PATH_REMOVE_DIRECTORY | __WASI_RIGHTS_PATH_UNLINK_FILE)

/* Full grant and its read-only derivation. */
#define WASI_RIGHTS_ALL ((uint64_t)-1)
#define WASI_RIGHTS_READONLY (WASI_RIGHTS_ALL & ~WASI_RIGHTS_WRITE)

/* stdio grant: every right except the seek/tell a stream cannot honour. */
#define WASI_RIGHTS_STDIO                                                      \
    (WASI_RIGHTS_ALL & ~(__WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL))

#define __WASI_PREOPENTYPE_DIR 0
#define __WASI_EVENTTYPE_CLOCK 0

typedef struct __wasi_fdstat_t {
    __wasi_filetype_t fs_filetype;
    __wasi_fdflags_t fs_flags;
    __wasi_rights_t fs_rights_base;
    __wasi_rights_t fs_rights_inheriting;
} __wasi_fdstat_t;

typedef struct __wasi_filestat_t {
    __wasi_device_t dev;
    __wasi_inode_t ino;
    __wasi_filetype_t filetype;
    __wasi_linkcount_t nlink;
    __wasi_filesize_t size;
    __wasi_timestamp_t atim;
    __wasi_timestamp_t mtim;
    __wasi_timestamp_t ctim;
} __wasi_filestat_t;

typedef struct __wasi_subscription_t {
    __wasi_userdata_t userdata;
    __wasi_eventtype_t type;
    union {
        struct {
            __wasi_clockid_t id;
            __wasi_timestamp_t timeout;
            __wasi_timestamp_t precision;
            __wasi_subclockflags_t flags;
        } clock;
    } u;
} __wasi_subscription_t;

/* WASI snapshot-preview1 __wasi_event_t is 32 bytes. poll_oneoff writes it at
 * fixed offsets (see wasi_poll_oneoff in wasi-vfs.c) for a clock subscription;
 * fd_read/fd_write subscriptions return NOSYS. It stays an opaque byte block
 * here. */
typedef struct __wasi_event_t {
    uint8_t _pad[32];
} __wasi_event_t;
