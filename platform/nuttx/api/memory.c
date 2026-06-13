/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX platform memory stats.
 *
 * NuttX exposes mallinfo() (not the glibc-only mallinfo2()); its uordblks/arena
 * fields are int-width, which is sufficient on 32-bit targets (heap < 2 GB). */

#include <malloc.h>

#include <platform.h>

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    struct mallinfo mi = mallinfo();
    if (heap_used) {
        *heap_used = (size_t)mi.uordblks;
    }
    if (heap_total) {
        *heap_total = (size_t)mi.arena;
    }
}

const char *PlatformName(void) { return "nuttx"; }
