/* SPDX-License-Identifier: Apache-2.0 */

/* Wake descriptors backed by eventfd. A stop raises one before its signal, so a
 * wait the signal missed, between two sleeps, still ends at its next check. */

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <debug_trace.h>
#include <platform.h>

int PlatformWakeCreate(void) {
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    if (fd < 0) {
        DEBUG_TRACE("eventfd create failed: %d", errno);
        return -1;
    }
    return fd;
}

void PlatformWakeRaise(int fd) {
    uint64_t one = 1;

    if (fd < 0) {
        return;
    }
    /* Nothing drains the counter, so the descriptor stays readable. */
    if (write(fd, &one, sizeof(one)) < 0) {
        DEBUG_TRACE("eventfd raise failed: %d", errno);
    }
}

bool PlatformWakeRaised(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};

    if (fd < 0) {
        return false;
    }
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) != 0;
}

void PlatformWakeClose(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
