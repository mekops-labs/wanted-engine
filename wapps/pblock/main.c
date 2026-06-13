/* SPDX-License-Identifier: Apache-2.0 */

/* pblock — blocks indefinitely on a read from an empty pipe.
 *
 * Where blocker parks in a single timed sleep, pblock parks in fd_read on a
 * /dev/pipe channel that has no writer, so the read never completes on its own:
 * the engine's pipe poll loop times out to EAGAIN and the wapp re-reads forever.
 * The only thing that ends it is the supervisor's stop interrupting the blocked
 * host call (the terminate flag is honoured once the interrupted read returns to
 * the interpreter). It must be reaped promptly; if the interrupt does not work
 * the wapp blocks until the test's external timeout — a hard failure, not a hang
 * papered over by a self-return. */

#include <fcntl.h>
#include <unistd.h>

#define CHAN "/dev/pipe/void"

int main(void) {
    char buf[64];
    int fd = open(CHAN, O_RDONLY);
    if (fd < 0)
        return 1;

    /* No writer ever attaches, so every read returns EAGAIN (negative); loop so
     * the wapp can only be ended by the stop interrupt, never by a self-return. */
    while (read(fd, buf, sizeof(buf)) < 0) {
    }

    close(fd);
    return 0;
}
