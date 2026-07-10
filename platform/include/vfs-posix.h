/* SPDX-License-Identifier: Apache-2.0 */

/* Shared POSIX <-> VFS conversions used by the platform VFS drivers. The
 * directory-entry type map (convertDirtype) stays per-platform — NuttX lacks
 * DT_LNK/DT_SOCK/DT_WHT. */

#ifndef PLATFORM_VFS_POSIX_H
#define PLATFORM_VFS_POSIX_H

#include <fcntl.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>

#include <vfs.h>

static inline vfs_filetype_t convertFiletype(uint32_t t) {
    switch (t & S_IFMT) {
    case S_IFSOCK:
        return VFS_FILETYPE_SOCKET_STREAM;
    case S_IFLNK:
        return VFS_FILETYPE_SYMBOLIC_LINK;
    case S_IFREG:
        return VFS_FILETYPE_REGULAR_FILE;
    case S_IFBLK:
        return VFS_FILETYPE_BLOCK_DEVICE;
    case S_IFDIR:
        return VFS_FILETYPE_DIRECTORY;
    case S_IFCHR:
        return VFS_FILETYPE_CHARACTER_DEVICE;
    case S_IFIFO:
        return VFS_FILETYPE_REGULAR_FILE;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

/* O_DSYNC/O_RSYNC (synchronised I/O) are absent from esp-idf's newlib
 * fcntl.h — fall back to 0 (no-op bit) so this shared header compiles on
 * every platform target. */
#ifndef O_DSYNC
#define O_DSYNC 0
#endif
#ifndef O_RSYNC
#define O_RSYNC 0
#endif

static inline int convertVfsFlags(vfs_oflags_t f) {
    int flags =
        ((f & VFS_O_CREAT) ? O_CREAT : 0) |
        ((f & VFS_O_DIRECTORY) ? O_DIRECTORY : 0) |
        ((f & VFS_O_EXCL) ? O_EXCL : 0) | ((f & VFS_O_TRUNC) ? O_TRUNC : 0) |
        ((f & VFS_O_APPEND) ? O_APPEND : 0) |
        ((f & VFS_O_NONBLOCK) ? O_NONBLOCK : 0) |
        ((f & VFS_O_DSYNC) ? O_DSYNC : 0) | ((f & VFS_O_SYNC) ? O_SYNC : 0) |
        ((f & VFS_O_RSYNC) ? O_RSYNC : 0) | ((f & VFS_O_RDWR) ? O_RDWR : 0) |
        ((f & VFS_O_WRONLY) ? O_WRONLY : 0) |
        ((f & VFS_O_RDONLY) ? O_RDONLY : 0);

    return flags;
}

static inline uint64_t convertTimespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((uint64_t)ts->tv_sec >= UINT64_MAX / 1000000000ULL)
        return UINT64_MAX;
    return ((uint64_t)ts->tv_sec * 1000000000ULL) + ts->tv_nsec;
}

#endif /* PLATFORM_VFS_POSIX_H */
