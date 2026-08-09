/* SPDX-License-Identifier: Apache-2.0 */

/* cpuhog — a never-yielding busy loop with no sleep and no syscalls. It proves
 * the engine can stop a wapp that never yields: WAMR checks the terminate flag
 * per instruction, so a "stop" unwinds the in-flight call regardless. */

#include <string.h>
#include <unistd.h>

#define MARKER "cpuhog-was-here\n"

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    volatile unsigned long x = 0;
    for (;;)
        x++;                                        /* never yields */
    return 0;
}
