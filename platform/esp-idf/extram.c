/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF PlatformExtram* backend: routes the engine's general allocator
 * (WantedMalloc, wanted_malloc.c) into PSRAM via heap_caps_malloc. Safe under
 * concurrent flash writes and never ISR-touched. A caller needing internal
 * RAM (hot-path vfs_ctx_t/wasi_ctx_t) calls malloc() directly instead, per
 * wanted_malloc.c's convention.
 *
 * Allocations are explicitly 8-byte aligned: plain heap_caps_malloc on the
 * classic ESP32's PSRAM heap does not guarantee alignment beyond its block
 * granularity (not a multiple of 8), which fails WAMR's GC allocator (it
 * requires 8-byte alignment on its heap-struct buffer). */

#include <stddef.h>
#include <string.h>

#include <platform.h>

#include "esp_heap_caps.h"

#define EXTRAM_ALIGN 8u
#define EXTRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void *PlatformExtramMalloc(size_t size) {
    return heap_caps_aligned_alloc(EXTRAM_ALIGN, size, EXTRAM_CAPS);
}

/* heap_caps has no aligned-realloc: an in-place grow could shift the block's
 * effective alignment, and there is no way to ask it to preserve one. Alloc
 * fresh (aligned) + copy + free instead of heap_caps_realloc(). */
void *PlatformExtramRealloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return PlatformExtramMalloc(size);
    }
    if (size == 0) {
        heap_caps_free(ptr);
        return NULL;
    }

    size_t oldSize = heap_caps_get_allocated_size(ptr);
    void *p = heap_caps_aligned_alloc(EXTRAM_ALIGN, size, EXTRAM_CAPS);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, ptr, oldSize < size ? oldSize : size);
    heap_caps_free(ptr);
    return p;
}

void PlatformExtramFree(void *ptr) { heap_caps_free(ptr); }
