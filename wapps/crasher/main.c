/* SPDX-License-Identifier: Apache-2.0 */

/* crasher — exits immediately, every time.
 *
 * Models a wapp stuck in a crash loop (one that dies the moment it starts, e.g.
 * from a fatal config error). The selftest supervisor restarts it rapidly to
 * exercise the start/reap cycle under churn: the engine must reclaim the slot
 * each time, stay responsive, and not thrash or leak resources across cycles. */

#include <string.h>
#include <unistd.h>

#define MARKER "crasher\n"

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    return 0;                                        /* die immediately */
}
