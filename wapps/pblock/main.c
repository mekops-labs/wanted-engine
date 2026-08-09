/* SPDX-License-Identifier: Apache-2.0 */

/* pblock — parks in fd_read on a /dev/pipe channel with no writer, so the read
 * never completes on its own. Only the supervisor's stop, interrupting the
 * blocked host call, ends it; it must be reaped promptly. */

#include <fcntl.h>
#include <unistd.h>

#define CHAN "/dev/pipe/void"

int main(void) {
    char buf[64];
    int fd = open(CHAN, O_RDONLY);
    if (fd < 0)
        return 1;

    /* No writer ever attaches, so every read returns EAGAIN (negative); loop so
     * the wapp can only be ended by the stop interrupt, not a self-return. */
    while (read(fd, buf, sizeof(buf)) < 0) {
    }

    close(fd);
    return 0;
}
