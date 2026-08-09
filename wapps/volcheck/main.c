/* SPDX-License-Identifier: Apache-2.0 */

/* volcheck — exercises an engine-managed volume's persistence. Launched with a
 * `volume` mount at /data, it writes a marker on a fresh store and reads it
 * back on a populated one, so relaunching proves the volume survives. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define STATE_PATH "/data/state"
#define PAYLOAD    "persist-42"

int main(void) {
    char buf[64];

    int fd = open(STATE_PATH, O_RDONLY);
    if (fd >= 0) {
        int n = read(fd, buf, sizeof(buf));
        close(fd);
        if (n < 0)
            n = 0;
        /* "vol-open" reports the marker survived a restart and re-opened;
         * "vol-read:<bytes>" reports the content read back. The supervisor
         * checks them separately, so an open that reads nothing is visible. */
        write(STDOUT_FILENO, "vol-open\n", 9);
        write(STDOUT_FILENO, "vol-read:", 9);
        write(STDOUT_FILENO, buf, (size_t)n);
        write(STDOUT_FILENO, "\n", 1);
        return 0;
    }

    fd = open(STATE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        write(STDOUT_FILENO, "vol-fail\n", 9);
        return 1;
    }
    write(fd, PAYLOAD, strlen(PAYLOAD));
    close(fd);
    write(STDOUT_FILENO, "vol-wrote\n", 10);
    return 0;
}
