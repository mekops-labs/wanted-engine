/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"

#include <platform.h>

/* ESP-IDF power backing. A light sleep keeps memory and returns where it
 * stopped; a deep sleep resets the CPU. The saved ladder sits in RTC slow
 * memory, which a deep sleep keeps and a cold start leaves undefined, so a
 * header with a magic and a length guards it. */

#define ESP_POWER_MAGIC 0x50575253U
#define ESP_POWER_STATE_MAX 512

typedef struct power_block_t {
    uint32_t magic;
    uint32_t len;
    uint64_t storedUs;
    uint8_t data[ESP_POWER_STATE_MAX];
} power_block_t;

static RTC_NOINIT_ATTR power_block_t block;

static void armTimer(uint64_t timerUs) {
    if (timerUs == 0)
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
    else
        esp_sleep_enable_timer_wakeup(timerUs);
}

/* A refused sleep reports why: a timer too short to sleep on is a timer wake,
 * and a wake source that fired before the sleep is an external one. */
plat_power_wake_t PlatformPowerLightSleep(uint64_t timerUs) {
    armTimer(timerUs);
    esp_err_t err = esp_light_sleep_start();
    if (err == ESP_ERR_SLEEP_TOO_SHORT_SLEEP_DURATION)
        return PLAT_POWER_WAKE_TIMER;
    if (err != ESP_OK)
        return PLAT_POWER_WAKE_EXTERNAL;
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER
               ? PLAT_POWER_WAKE_TIMER
               : PLAT_POWER_WAKE_EXTERNAL;
}

void PlatformPowerDeepSleep(uint64_t timerUs) {
    armTimer(timerUs);
    esp_deep_sleep_start();
}

plat_power_boot_t PlatformPowerBootCause(void) {
    switch (esp_sleep_get_wakeup_cause()) {
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return PLAT_POWER_BOOT_COLD;
    case ESP_SLEEP_WAKEUP_TIMER:
        return PLAT_POWER_BOOT_TIMER;
    default:
        return PLAT_POWER_BOOT_EXTERNAL;
    }
}

bool PlatformPowerStateStore(const void *buf, size_t len) {
    if (buf == NULL || len > sizeof(block.data))
        return false;
    memcpy(block.data, buf, len);
    block.len = (uint32_t)len;
    block.storedUs = esp_clk_rtc_time();
    block.magic = ESP_POWER_MAGIC;
    return true;
}

bool PlatformPowerStateLoad(void *buf, size_t len, uint64_t *elapsedUs) {
    if (buf == NULL || block.magic != ESP_POWER_MAGIC || block.len != len)
        return false;
    memcpy(buf, block.data, len);
    if (elapsedUs != NULL)
        *elapsedUs = esp_clk_rtc_time() - block.storedUs;
    block.magic = 0;
    return true;
}
