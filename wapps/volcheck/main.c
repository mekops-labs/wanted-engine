/* SPDX-License-Identifier: Apache-2.0 */

/* volcheck — exercises an engine-managed volume's persistence.
 *
 * It is launched with a `volume` mount at /data. On each run it looks for its
 * marker file: absent → a fresh store, so it writes the marker and reports
 * "vol-wrote"; present → a store that already holds state, so it reads the
 * marker back and reports "vol-read:<payload>". Relaunching the same instance
 * proves the volume survives a restart. The report goes to the log console; the
 * selftest supervisor reads it back and asserts persistence. */

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
        /* -> log console. "vol-open" reports the marker file survived a restart
         * and re-opened (the persistence guarantee); "vol-read:<bytes>" reports
         * the content read back through the preopen, which the supervisor checks
         * separately so a host-fs that opens but reads back nothing is visible. */
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
