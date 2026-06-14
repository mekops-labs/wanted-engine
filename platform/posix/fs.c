/* SPDX-License-Identifier: Apache-2.0 */

/* Shared POSIX filesystem primitives: preopen state dirs and the openat-class
 * rename/mkdir used by the VFS. renameat/mkdirat are unconditional on NuttX; on
 * a host/glibc build they need the feature-test macros below. */

#ifndef __NuttX__
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <platform.h>

/* mkdir -p: walk the path, creating each missing component. Existing
 * components are tolerated (EEXIST → OK); any other mkdir failure aborts. */
static int mkdir_p(const char *path, mode_t mode) {
    if (!path || !*path)
        return -EINVAL;
    char buf[1024];
    size_t plen = strlen(path);
    if (plen >= sizeof(buf))
        return -ENAMETOOLONG;
    memcpy(buf, path, plen + 1);
    for (size_t i = 1; i < plen; i++) {
        if (buf[i] != '/')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, mode) != 0 && errno != EEXIST)
            return -errno;
        buf[i] = '/';
    }
    if (mkdir(buf, mode) != 0 && errno != EEXIST)
        return -errno;
    return 0;
}

int PlatformOpenStateDir(const char *path, bool readonly) {
    if (!path || !*path)
        return -EINVAL;
    if (!readonly) {
        int rc = mkdir_p(path, 0700);
        if (rc < 0)
            return rc;
    }
    /* The directory fd carries no write intent; per-file write capability is
     * granted (or, for a read-only mount, denied) by the VFS platform driver. */
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd < 0)
        return -errno;
    return fd;
}

int PlatformFsRename(int old_fd, const char *old_path,
                     int new_fd, const char *new_path) {
    if (!old_path || !new_path)
        return -EINVAL;
    if (renameat(old_fd, old_path, new_fd, new_path) < 0)
        return -errno;
    return 0;
}

int PlatformFsMkdir(int fd, const char *path) {
    if (!path)
        return -EINVAL;
    if (mkdirat(fd, path, 0755) < 0)
        return -errno;
    return 0;
}
