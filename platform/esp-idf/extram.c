/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF PlatformExtram* backend: routes the engine's general allocator
 * (WantedMalloc, wanted_malloc.c) into PSRAM via heap_caps_malloc. Safe under
 * concurrent flash writes and never ISR-touched.
 * A caller needing internal RAM (the hot-path vfs_ctx_t/wasi_ctx_t contexts)
 * calls malloc() directly instead of WantedMalloc(), per the existing
 * convention (wanted_malloc.c's own comment). */

#include <stddef.h>

#include <platform.h>

#include "esp_heap_caps.h"

void *PlatformExtramMalloc(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void *PlatformExtramRealloc(void *ptr, size_t size) {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void PlatformExtramFree(void *ptr) { heap_caps_free(ptr); }
