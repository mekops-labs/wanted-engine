/* SPDX-License-Identifier: Apache-2.0 */

/* duplex — one image, two inter-wapp pipe roles selected by the ROLE env var,
 * proving /dev/pipe is a process-wide channel between separate namespaces. The
 * selftest stages this source under two registry names and launches both. */
#include <errno.h>
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

    if (role != NULL && strcmp(role, "rtreader") == 0) {
        /* The grant arrives while this runs: retry the open until it does,
         * read once, then reopen the name until the supervisor's revoke lands. */
        char got[64];
        int fd = -1;
        for (int i = 0; i < 100 && fd < 0; i++) {
            fd = open(CHAN, O_RDONLY);
            if (fd < 0)
                usleep(50000);
        }
        if (fd < 0)
            return 1;
        int n = read(fd, got, sizeof(got));
        if (n <= 0)
            return 1;
        write(STDOUT_FILENO, got, n);
        int denied = 0;
        for (int i = 0; i < 150 && !denied; i++) {
            int again = open(CHAN, O_RDONLY);
            if (again >= 0) {
                close(again);
                usleep(200000);
            } else {
                denied = errno == ENOENT;
            }
        }
        const char *verdict = denied ? "\nreopen: denied\n" : "\nreopen: allowed\n";
        write(STDOUT_FILENO, verdict, strlen(verdict));
        close(fd);
        return denied ? 0 : 2;
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
