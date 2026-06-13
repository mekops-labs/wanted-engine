/* SPDX-License-Identifier: Apache-2.0 */

/* hello — a minimal WASI sample wapp with three behaviours selected by the
 * ROLE environment variable, passed in via its launch config (params.envs). It
 * talks to the outside world only through the VFS its launch config grants it.
 *
 *   ROLE=writer: open /dev/pipe/smoke and write PAYLOAD, then exit.
 *   ROLE=reader: blocking-read /dev/pipe/smoke and copy what it received to a
 *                host result file (a preopen), then exit. The result file —
 *                not stdout — is the observation point, because the stdio
 *                teardown drops a launched wapp's late stdout.
 *   ROLE unset:  write an alive marker, stay alive briefly so a concurrent
 *                `status` sees it, then write an exit marker. Used by the
 *                multi-wapp concurrency smoke test.
 *
 * The reader/writer pair proves /dev/pipe is an inter-wapp IPC channel.
 */
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PIPE_PATH   "/dev/pipe/smoke"
#define RESULT_DIR  "/tmp/wanted-smoke-pipe"
#define RESULT_PATH RESULT_DIR "/result"

#define PAYLOAD     "inter-wapp-pipe-ok"
#define ROLE_ENV    "ROLE"
#define ROLE_WRITER "writer"
#define ROLE_READER "reader"

#define MARKER_ALIVE  "hello-wapp: alive\n"
#define MARKER_EXIT   "hello-wapp: exit\n"
#define ALIVE_SECONDS 2

static int RunWriter(void) {
    int fd = open(PIPE_PATH, O_WRONLY);
    if (fd < 0)
        return 1;
    write(fd, PAYLOAD, strlen(PAYLOAD));
    close(fd);
    return 0;
}

static int RunReader(void) {
    int fd = open(PIPE_PATH, O_RDONLY);
    if (fd < 0)
        return 1;

    char   buf[64];
    size_t total = 0;
    /* The read blocks until the writer attaches and produces data; loop until
     * the full payload arrives or the writer closes (EOF / error). */
    while (total < sizeof(buf) - 1) {
        int n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
        if (total >= strlen(PAYLOAD))
            break;
    }
    close(fd);

    int out = open(RESULT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0)
        return 1;
    write(out, buf, total);
    close(out);
    return 0;
}

int main(void) {
    const char *role = getenv(ROLE_ENV);
    if (role == NULL)
        role = "";

    if (strcmp(role, ROLE_WRITER) == 0)
        return RunWriter();
    if (strcmp(role, ROLE_READER) == 0)
        return RunReader();

    write(STDOUT_FILENO, MARKER_ALIVE, strlen(MARKER_ALIVE));
    sleep(ALIVE_SECONDS);
    write(STDOUT_FILENO, MARKER_EXIT, strlen(MARKER_EXIT));
    return 0;
}
