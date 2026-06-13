/* SPDX-License-Identifier: Apache-2.0 */

/* cpuhog — a never-yielding busy loop with no sleep and no syscalls.
 *
 * Where `looper` sleeps between iterations (going off-CPU), cpuhog spins on a
 * pure compute loop that never blocks. It proves the engine can stop even a
 * wapp that never yields: WAMR checks the termination flag per instruction in
 * the interpreter loop, so a control-plane "stop" unwinds the in-flight call
 * (wasm_runtime_terminate) without the wapp ever cooperating. The supervisor
 * starts it, confirms it runs, stops it, and confirms it terminated. */

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
