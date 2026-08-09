/* SPDX-License-Identifier: Apache-2.0 */

/* blocker — parks in a single long host call, where there are no instruction
 * boundaries at which to check the terminate flag. The engine must stay
 * responsive and the wapp must not leak, whether or not the stop interrupts. */

#include <string.h>
#include <unistd.h>

#define MARKER "blocker-was-here\n"
#define BLOCK_SECONDS 5

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    sleep(BLOCK_SECONDS);                            /* one long host call */
    return 0;
}
