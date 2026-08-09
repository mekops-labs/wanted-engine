/* SPDX-License-Identifier: Apache-2.0 */

/* stackbomb — unbounded recursion that overflows the WASM stack. Each frame's
 * on-stack buffer escapes into the callee, so the call cannot be flattened into
 * a loop. The engine must trap it and keep everything else running. */

#include <string.h>
#include <unistd.h>

#define MARKER "stackbomb-was-here\n"

static volatile char sink;

/* noinline stops the compiler collapsing the recursion chain. */
__attribute__((noinline)) static void recurse(int depth, const char *prev) {
    char frame[256];
    frame[0] = (char)depth;
    frame[1] = prev ? prev[0] : 0;  /* touch the previous frame */
    sink = frame[1];                /* observable side effect */
    recurse(depth + 1, frame);      /* pass our frame down -> real recursion */
}

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    recurse(0, 0);                                  /* stack guard -> trap */
    return 0;
}
