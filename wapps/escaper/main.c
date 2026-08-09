/* SPDX-License-Identifier: Apache-2.0 */

/* escaper — attempts the classic sandbox escapes (parent traversal, absolute
 * host paths, writing the read-only image, reaching the control plane), and
 * reports "sandbox-OK" or "sandbox-LEAK". Every one must be denied. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define OK_MARKER   "sandbox-OK\n"
#define LEAK_MARKER "sandbox-LEAK\n"

/* Open that must fail. Returns 1 if the path was unexpectedly opened. */
static int leaked(const char *path, int flags) {
    int fd = open(path, flags);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

int main(void) {
    int leak = 0;

    /* parent traversal past the sandbox root */
    leak |= leaked("/../../../../../../etc/passwd", O_RDONLY);
    leak |= leaked("../../../../../../etc/passwd", O_RDONLY);
    /* absolute host path — no host preopen is bound into this namespace */
    leak |= leaked("/etc/passwd", O_RDONLY);
    /* write the read-only TarFS image */
    leak |= leaked("/app.wasm", O_WRONLY);
    /* reach the engine control plane / another wapp (not in this namespace) */
    leak |= leaked("/dev/wanted/ctl", O_WRONLY);
    leak |= leaked("/dev/wanted/wapps/supervisor/ctl", O_WRONLY);

    const char *verdict = leak ? LEAK_MARKER : OK_MARKER;
    write(STDOUT_FILENO, verdict, strlen(verdict));   /* -> log console */
    return leak ? 1 : 0;
}
