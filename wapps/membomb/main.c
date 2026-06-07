/* membomb — allocate until allocation fails.
 *
 * The wapp's linear memory is capped (see Makefile --max-memory), so malloc
 * cannot grow the heap past the sandbox bound: it eventually returns NULL
 * inside this instance instead of exhausting host RAM. Each chunk is linked
 * into an escaping global list so the allocations stay live (the optimizer
 * cannot delete the loop as dead) and the loop terminates the moment malloc
 * returns NULL. The selftest supervisor asserts the bound is per-wapp — the
 * wapp ends in a dead state while the supervisor and host survive (no OOM, no
 * host crash). */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MARKER  "membomb-was-here\n"
#define BOUNDED "membomb-bounded\n"
#define CHUNK   512

static void *volatile head;           /* escaping list head: chunks stay live */
static unsigned long volatile total;  /* observable progress */

int main(void) {
    write(STDOUT_FILENO, MARKER, strlen(MARKER));   /* -> log console */
    for (;;) {
        void **node = malloc(CHUNK);
        if (!node)
            break;                    /* bounded by the linear-memory cap */
        node[0] = head;               /* link it so the chunk cannot be elided */
        head = node;
        total = total + 1;
    }
    write(STDOUT_FILENO, BOUNDED, strlen(BOUNDED));  /* hit the bound, exiting */
    return 0;
}
