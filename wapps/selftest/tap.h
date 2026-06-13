/* SPDX-License-Identifier: Apache-2.0 */

/* Minimal TAP (Test Anything Protocol) emitter for in-WASM test wapps.
 *
 * Output goes to stdout, which the engine wires to the platform console on
 * every target (Linux CLI, NuttX sim, future HW over UART), so the host runner
 * just scans the console for `ok`/`not ok`/`1..N`. No engine natives, only
 * WASI writes — identical everywhere. Counting is process-global; call
 * tap_plan() last. */

#ifndef WANTED_TAP_H
#define WANTED_TAP_H

#include <stdio.h>

static int tap_count = 0;
static int tap_fail = 0;

/* Record one assertion. Returns `cond` so callers can branch if needed. */
static int tap_ok(int cond, const char *desc) {
    tap_count++;
    if (cond) {
        printf("ok %d - %s\n", tap_count, desc);
    } else {
        printf("not ok %d - %s\n", tap_count, desc);
        tap_fail++;
    }
    fflush(stdout);
    return cond;
}

/* Emit a TAP diagnostic comment (ignored by TAP parsers, visible on the
 * console). Marks progress so a phase that sleeps for several seconds is not
 * mistaken for a hang. */
static void tap_diag(const char *msg) {
    printf("# %s\n", msg);
    fflush(stdout);
}

/* Emit the plan line and return a process exit code (0 = all passed). */
static int tap_plan(void) {
    printf("1..%d\n", tap_count);
    fflush(stdout);
    return tap_fail == 0 ? 0 : 1;
}

#endif /* WANTED_TAP_H */
