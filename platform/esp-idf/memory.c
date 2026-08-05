/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform memory stats. heap_caps aggregates internal DRAM and, when
 * CONFIG_SPIRAM_USE_MALLOC is set, the PSRAM pool — matching where the engine's
 * allocations land. */

#include <errno.h>

#include <platform.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    size_t total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t freeb = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    if (heap_total)
        *heap_total = total;
    if (heap_used)
        *heap_used = (total > freeb) ? total - freeb : 0;
}

/* Registry and volumes live on the "persist" littlefs partition the app mounts
 * at boot; 0 when it is unmounted. The label must match app_main.c's. */
#define PERSIST_PARTITION_LABEL "persist"

void PlatformStorageStats(size_t *free_b, size_t *total_b) {
    size_t total = 0, used = 0;
    if (free_b)
        *free_b = 0;
    if (total_b)
        *total_b = 0;
    if (esp_littlefs_info(PERSIST_PARTITION_LABEL, &total, &used) != ESP_OK)
        return;
    if (total_b)
        *total_b = total;
    if (free_b)
        *free_b = (total > used) ? total - used : 0;
}

const char *PlatformName(void) { return "esp-idf"; }

/* The toolchain stamps the ELF SHA-256 into the image descriptor at build time,
 * so this reads a digest rather than computing one. The raw field is encoded
 * here because esp_app_get_elf_sha256() truncates; see the platform guide. */
int PlatformFirmwareDigest(char *buf, size_t bufLen) {
    static const char hex[] = "0123456789abcdef";
    const esp_app_desc_t *desc = esp_app_get_description();

    if (desc == NULL)
        return -ENOSYS;

    /* Bound against the field itself, not the advertised length, so a wider
     * digest reports -ENOSPC instead of overrunning the caller. */
    size_t len = 2 * sizeof(desc->app_elf_sha256);
    if (bufLen < len + 1)
        return -ENOSPC;

    for (size_t i = 0; i < sizeof(desc->app_elf_sha256); i++) {
        buf[2 * i] = hex[desc->app_elf_sha256[i] >> 4];
        buf[2 * i + 1] = hex[desc->app_elf_sha256[i] & 0x0f];
    }
    buf[len] = '\0';
    return (int)len;
}
