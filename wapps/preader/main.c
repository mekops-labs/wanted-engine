/* SPDX-License-Identifier: Apache-2.0 */

/* preader — receives a payload from another wapp over a shared named pipe.
 *
 * /dev/pipe/<name> is a process-wide store, so this wapp and pwriter — in
 * separate namespaces — rendezvous on the same channel. preader blocks reading
 * the channel until pwriter writes, then echoes what it received to its log
 * console for the selftest supervisor to verify. Together they prove a payload
 * crosses the wapp boundary (the inter-wapp pipe round-trip). */

#include <fcntl.h>
#include <unistd.h>

#define CHAN "/dev/pipe/duplex"

int main(void) {
    char buf[64];
    int fd = open(CHAN, O_RDONLY);
    if (fd < 0)
        return 1;
    int n = read(fd, buf, sizeof(buf));   /* blocks (sleeps) until pwriter writes */
    close(fd);
    if (n <= 0)
        return 1;
    write(STDOUT_FILENO, buf, n);          /* -> log console */
    return 0;
}
