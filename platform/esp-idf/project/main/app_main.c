/*
 * ESP-IDF entry point for the WANTED engine.
 *
 * Brings the ESP-IDF platform layer up to a booting image that links the full
 * engine and the WAMR runtime. The engine is referenced (WantedStart) to force
 * it into the link, but not yet driven — the platform layer is still stubbed.
 */

#include "esp_log.h"
#include "esp_heap_caps.h"

#include "wanted.h"

#define TAG "wanted"

void app_main(void)
{
    ESP_LOGI(TAG, "WANTED engine — ESP-IDF platform bring-up");
    ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI(TAG, "engine entry WantedStart @%p (not yet driven)", (void *)WantedStart);
}
