/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <sys/select.h>
#include <sys/time.h>

#include <platform.h>

void PlatformFdReady(int fd, bool *readable, bool *writable) {
    fd_set r;
    fd_set w;
    struct timeval tv = {0, 0};

    *readable = true;
    *writable = true;
    if (fd < 0) {
        return;
    }
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(fd, &r);
    FD_SET(fd, &w);
    /* A file on a filesystem without select support never blocks. */
    if (select(fd + 1, &r, &w, NULL, &tv) < 0) {
        return;
    }
    *readable = FD_ISSET(fd, &r);
    *writable = FD_ISSET(fd, &w);
}
