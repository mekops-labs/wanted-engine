/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform memory stats. heap_caps aggregates internal DRAM and, when
 * CONFIG_SPIRAM_USE_MALLOC is set, the PSRAM pool — matching where the engine's
 * allocations land. */

#include <platform.h>

#include "esp_heap_caps.h"
#include "esp_spiffs.h"

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (heap_total)
        *heap_total = total;
    if (heap_used)
        *heap_used = (total > freeb) ? total - freeb : 0;
}

/* Registry and volumes live on the data partition; 0 when it is unmounted. */
void PlatformStorageStats(size_t *free_b, size_t *total_b) {
    size_t total = 0, used = 0;
    if (free_b)
        *free_b = 0;
    if (total_b)
        *total_b = 0;
    if (esp_spiffs_info(NULL, &total, &used) != ESP_OK)
        return;
    if (total_b)
        *total_b = total;
    if (free_b)
        *free_b = (total > used) ? total - used : 0;
}

const char *PlatformName(void) { return "esp-idf"; }
