/* SPDX-License-Identifier: Apache-2.0 */

/* Unit-test platform: no external RAM, so the PSRAM allocator is the ordinary
 * heap. Keeps PlatformExtram* resolvable in the test build. */

#include <stdlib.h>

#include <platform.h>

void *PlatformExtramMalloc(size_t size) { return malloc(size); }
void *PlatformExtramRealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}
void PlatformExtramFree(void *ptr) { free(ptr); }
