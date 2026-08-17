/* SPDX-License-Identifier: Apache-2.0 */

/* Wake descriptors backed by a pipe, so the host suite can exercise a wait that
 * a stop ends. The read end is the descriptor a wait watches. */

#include <errno.h>
#include <stdbool.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <platform.h>

/* Write ends, indexed by the read end this returns. A wapp holds one wake
 * descriptor, and the suite runs a handful at a time. */
#define WAKE_MAX_FDS 64

static int writeEnd[WAKE_MAX_FDS];
static int initialised;

static void initTable(void) {
    if (initialised) {
        return;
    }
    for (int i = 0; i < WAKE_MAX_FDS; i++) {
        writeEnd[i] = -1;
    }
    initialised = 1;
}

int PlatformWakeCreate(void) {
    int fds[2];

    initTable();
    if (pipe(fds) < 0) {
        return -1;
    }
    if (fds[0] < 0 || fds[0] >= WAKE_MAX_FDS) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    writeEnd[fds[0]] = fds[1];
    return fds[0];
}

void PlatformWakeRaise(int fd) {
    initTable();
    if (fd < 0 || fd >= WAKE_MAX_FDS || writeEnd[fd] < 0) {
        return;
    }
    /* One byte is enough: the wait watches for readability and never drains. */
    const char one = 1;
    if (write(writeEnd[fd], &one, 1) < 0) {
        return;
    }
}

bool PlatformWakeRaised(int fd) {
    fd_set r;
    struct timeval tv = {0, 0};

    if (fd < 0) {
        return false;
    }
    FD_ZERO(&r);
    FD_SET(fd, &r);
    return select(fd + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(fd, &r);
}

void PlatformWakeClose(int fd) {
    initTable();
    if (fd < 0 || fd >= WAKE_MAX_FDS) {
        return;
    }
    if (writeEnd[fd] >= 0) {
        close(writeEnd[fd]);
        writeEnd[fd] = -1;
    }
    close(fd);
}
