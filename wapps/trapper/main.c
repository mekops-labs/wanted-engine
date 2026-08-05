/* SPDX-License-Identifier: Apache-2.0 */

/* trapper — prints a marker, then reads far outside its linear memory to force
 * a WASM out-of-bounds trap. The engine must catch it, end this wapp in a dead
 * state, and keep the supervisor and other wapps running. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define MARKER "trapper-was-here\n"

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));            /* -> log console */
    volatile int *p = (volatile int *)(uintptr_t)0x7fff0000; /* well past 64 KB */
    return *p;                                               /* OOB read -> trap */
}
