/* SPDX-License-Identifier: Apache-2.0 */

/* pwriter — sends a payload to another wapp over a shared named pipe.
 *
 * Writes a known payload to /dev/pipe/duplex, the process-wide channel preader
 * is blocked reading. The selftest supervisor launches preader first, then this
 * wapp, and checks preader's log for the payload. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define CHAN    "/dev/pipe/duplex"
#define PAYLOAD "duplex-ok"

int main(void) {
    int fd = open(CHAN, O_WRONLY);
    if (fd < 0)
        return 1;
    write(fd, PAYLOAD, strlen(PAYLOAD));
    close(fd);
    return 0;
}
