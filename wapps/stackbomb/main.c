/* stackbomb — unbounded recursion that overflows the WASM stack.
 *
 * Prints a marker (captured by the engine's log console), then recurses without
 * a base case. Each frame keeps a real on-stack buffer and passes its address
 * to the next call, which reads it: because the buffer escapes into the callee,
 * the call is not in a tail position the optimizer can flatten into a loop, so
 * it stays genuine recursion and the C shadow stack grows every call until it
 * crosses the guard. The engine must trap that access, end this wapp in a dead
 * state, and keep the supervisor and other wapps running. Launched by the
 * selftest supervisor, which asserts the containment. */

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
