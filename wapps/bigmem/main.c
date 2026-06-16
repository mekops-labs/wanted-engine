/* SPDX-License-Identifier: Apache-2.0 */

/* bigmem — grows its linear memory past one page to exercise the engine's
 * WASM_MAX_MEMORY_PAGES cap. It allocates ~160 KiB (≈3 pages) in chunks until it
 * reaches the target or malloc fails. (Any malloc pulls in memory.grow, so the
 * module is not collapsed by WAMR's shrunk-memory pass.)
 *
 * Under a cap that admits the growth it reaches the target and logs
 * "bigmem-reached"; under a 1-page cap the grow is refused, malloc returns NULL
 * early, and it logs "bigmem-bounded". The logged marker is the test's signal
 * (see test/memcap.sh). */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 4096
#define NEED (160 * 1024) /* > 2 pages, so it must grow past a 1-page cap */

int main(void) {
    void *volatile head = NULL; /* keep allocations live so none are elided */
    size_t got = 0;
    while (got < NEED) {
        void **node = malloc(CHUNK);
        if (!node)
            break; /* grow refused by the cap */
        node[0] = head;
        head = node;
        got += CHUNK;
    }
    const char *m = (got >= NEED) ? "bigmem-reached\n" : "bigmem-bounded\n";
    write(STDOUT_FILENO, m, strlen(m)); /* -> log console */
    return 0;
}
