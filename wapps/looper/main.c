/* SPDX-License-Identifier: Apache-2.0 */

/* looper — a well-behaved long-running test wapp.
 *
 * Loops forever so the supervisor can prove two things: that it runs
 * concurrently with the supervisor (both live at once), and that the engine
 * can stop it on request — a "stop" verb on its control node terminates the
 * in-flight WASM call (wasm_runtime_terminate) and the worker thread unwinds.
 * The sleep keeps it off the CPU between the engine's per-instruction
 * termination checks. */

#include <unistd.h>

int main(void) {
    for (;;)
        sleep(1);
    return 0;
}
