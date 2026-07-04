/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform memory stats. heap_caps aggregates internal DRAM and, when
 * CONFIG_SPIRAM_USE_MALLOC is set, the PSRAM pool — matching where the engine's
 * allocations land. */

#include <platform.h>

#include "esp_heap_caps.h"

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (heap_total)
        *heap_total = total;
    if (heap_used)
        *heap_used = (total > freeb) ? total - freeb : 0;
}

const char *PlatformName(void) { return "esp-idf"; }
