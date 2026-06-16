/* SPDX-License-Identifier: Apache-2.0 */

/* biginit — declares four initial linear-memory pages (see Makefile), used to
 * test the engine's load-time rejection of an image whose *initial* memory
 * exceeds WASM_MAX_MEMORY_PAGES (the runtime cap only bounds later growth).
 *
 * The small alloc loop keeps a memory.grow in the module so WAMR's
 * shrunk-memory pass does not collapse the declared four pages back to one.
 * Under a cap below four pages the engine refuses to load it (no marker);
 * under a cap of four or more it loads and logs "biginit-loaded". */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    void *volatile head = NULL;
    for (int i = 0; i < 4; i++) {
        void **node = malloc(4096);
        if (!node)
            break;
        node[0] = head;
        head = node;
    }
    static const char m[] = "biginit-loaded\n";
    write(STDOUT_FILENO, m, strlen(m)); /* -> log console */
    return 0;
}
