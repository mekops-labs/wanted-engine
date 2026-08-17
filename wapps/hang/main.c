/* SPDX-License-Identifier: Apache-2.0 */

/* hang — a wapp that spins without yielding, to reproduce the wedge a
 * misbehaving supervisor caused. With the task watchdog set to panic, the
 * board resets and the boot that follows carries the log that preceded it. */

int main(void) {
    volatile unsigned long long spin = 0;

    for (;;) {
        spin++;
    }
    return 0;
}
