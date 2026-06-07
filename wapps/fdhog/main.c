/* fdhog — exhausts a sandbox resource (file descriptors).
 *
 * Opens its read-only manifest over and over without closing, until the open
 * fails or a hard probe cap is reached, then reports on its log console. The
 * engine must bound the wapp (open eventually errors) and stay up — the abuse
 * must be contained to the wapp, never crash or exhaust the host. The selftest
 * supervisor asserts the wapp is reaped and the supervisor survives, and the
 * verdict (bounded vs. cap reached) is reported as a diagnostic. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define PROBE_CAP 1024           /* stop probing here so we never hang */
#define BOUNDED   "fdhog-bounded\n"
#define UNCAPPED  "fdhog-uncapped\n"

int main(void) {
    int opened = 0;
    for (int i = 0; i < PROBE_CAP; i++) {
        int fd = open("/manifest.json", O_RDONLY);
        if (fd < 0)
            break;               /* engine bounded the wapp's fd table */
        opened++;
    }
    const char *verdict = (opened < PROBE_CAP) ? BOUNDED : UNCAPPED;
    write(STDOUT_FILENO, verdict, strlen(verdict));   /* -> log console */
    return 0;
}
