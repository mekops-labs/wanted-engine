/* SPDX-License-Identifier: Apache-2.0 */

/* biginit — declares four initial linear-memory pages, testing the engine's
 * load-time rejection of an image whose initial memory exceeds the page cap.
 * It logs "biginit-loaded" only under a cap of four pages or more. */

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
