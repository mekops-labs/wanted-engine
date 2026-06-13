/* SPDX-License-Identifier: Apache-2.0 */

/* blocker — blocks inside a host syscall instead of executing WASM.
 *
 * cpuhog stresses a wapp busy in the interpreter, where the terminate flag is
 * checked on every instruction. blocker stresses the other axis: a wapp parked
 * in a single long host call, where there are no instruction boundaries to
 * check. It sleeps once for BLOCK_SECONDS, then exits. The selftest supervisor
 * launches it, issues a control-plane stop, and observes containment: on Linux
 * the stop interrupts the call; on the cooperative NuttX path it cannot, so the
 * wapp is only reaped when the call returns on its own. Either way the engine
 * must stay responsive and the wapp must not leak. */

#include <string.h>
#include <unistd.h>

#define MARKER "blocker-was-here\n"
#define BLOCK_SECONDS 5

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    sleep(BLOCK_SECONDS);                            /* one long host call */
    return 0;
}
