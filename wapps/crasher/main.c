/* SPDX-License-Identifier: Apache-2.0 */

/* crasher — exits immediately, every time, modelling a wapp stuck in a crash
 * loop. The selftest supervisor restarts it rapidly, so the engine must reclaim
 * the slot each time and neither thrash nor leak across cycles. */

#include <string.h>
#include <unistd.h>

#define MARKER "crasher\n"

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    return 0;                                        /* die immediately */
}
