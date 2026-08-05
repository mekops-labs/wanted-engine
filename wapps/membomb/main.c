/* SPDX-License-Identifier: Apache-2.0 */

/* membomb — allocates until allocation fails. Linear memory is capped, so
 * malloc returns NULL inside this instance rather than exhausting host RAM.
 * Each chunk escapes into a global list, so the loop is not optimized away. */

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
