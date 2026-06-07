/* trapper — a deliberately misbehaving test wapp.
 *
 * Prints a marker (captured by the engine's log console, not the platform
 * console), then reads far outside its linear memory (built with a 64 KB max)
 * to force a WASM out-of-bounds trap. The engine must catch the trap, end this
 * wapp in a dead state, and keep the supervisor and other wapps running.
 * Launched by the selftest supervisor, which asserts that containment and taps
 * the captured marker via the control plane. */

#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define MARKER "trapper-was-here\n"

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));            /* -> log console */
    volatile int *p = (volatile int *)(uintptr_t)0x7fff0000; /* well past 64 KB */
    return *p;                                               /* OOB read -> trap */
}
