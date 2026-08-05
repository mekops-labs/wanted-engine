/* SPDX-License-Identifier: Apache-2.0 */

/* fdhog — opens a read-only image file repeatedly without closing, until the
 * open fails or a probe cap is reached. The engine must bound the wapp and stay
 * up, containing the abuse rather than exhausting the host. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#define PROBE_CAP 1024           /* stop probing here so we never hang */
#define BOUNDED   "fdhog-bounded\n"
#define UNCAPPED  "fdhog-uncapped\n"

int main(void) {
    int opened = 0;
    for (int i = 0; i < PROBE_CAP; i++) {
        int fd = open("/app.wasm", O_RDONLY);
        if (fd < 0)
            break;               /* engine bounded the wapp's fd table */
        opened++;
    }
    const char *verdict = (opened < PROBE_CAP) ? BOUNDED : UNCAPPED;
    write(STDOUT_FILENO, verdict, strlen(verdict));   /* -> log console */
    return 0;
}
