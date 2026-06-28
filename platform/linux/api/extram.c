/* SPDX-License-Identifier: Apache-2.0 */

/* No external RAM on the Linux target — the PSRAM allocator is the ordinary
 * heap. Keeps PlatformExtram* resolvable so the shared engine/posix code links
 * on every platform. */

#include <stdlib.h>

#include <platform.h>

void *PlatformExtramMalloc(size_t size) { return malloc(size); }
void *PlatformExtramRealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}
void PlatformExtramFree(void *ptr) { free(ptr); }
