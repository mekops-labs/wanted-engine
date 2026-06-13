/* SPDX-License-Identifier: Apache-2.0 */

/* duplex — one image, two inter-wapp pipe roles selected by the ROLE env var
 * (passed via the launch config). Proves /dev/pipe is a process-wide channel
 * between wapps in separate namespaces. The selftest stages this single source
 * under two registry names (reader, writer) and launches each with its ROLE:
 *
 *   ROLE=writer:           write PAYLOAD to /dev/pipe/duplex, then exit.
 *   ROLE=reader (default): block-read /dev/pipe/duplex and echo what arrived to
 *                          the log console for the supervisor to verify.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHAN    "/dev/pipe/duplex"
#define PAYLOAD "duplex-ok"

int main(void) {
    const char *role = getenv("ROLE");

    if (role != NULL && strcmp(role, "writer") == 0) {
        int fd = open(CHAN, O_WRONLY);
        if (fd < 0)
            return 1;
        write(fd, PAYLOAD, strlen(PAYLOAD));
        close(fd);
        return 0;
    }

    /* reader (default) */
    char buf[64];
    int fd = open(CHAN, O_RDONLY);
    if (fd < 0)
        return 1;
    int n = read(fd, buf, sizeof(buf)); /* blocks until the writer writes */
    close(fd);
    if (n <= 0)
        return 1;
    write(STDOUT_FILENO, buf, n);       /* -> log console */
    return 0;
}
