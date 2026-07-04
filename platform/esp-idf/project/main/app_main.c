/*
 * ESP-IDF entry point for the WANTED engine.
 *
 * Exercises the ESP-IDF platform core primitives (name, memory stats, RNG,
 * monotonic clock + sleep, mutex) on-target and logs a pass/fail line each.
 * Driving the full engine (WantedStart) waits on the storage, VFS and wapp
 * layers.
 */

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"

#include <platform.h>

#define TAG "wanted"

static void selftest(void)
{
    ESP_LOGI(TAG, "platform: %s", PlatformName());

    size_t used = 0, total = 0;
    PlatformMemoryStats(&used, &total);
    ESP_LOGI(TAG, "memory: used=%u total=%u bytes", (unsigned)used, (unsigned)total);

    uint8_t a[16] = {0}, b[16] = {0};
    int64_t r1 = PlatfromGetRandom(a, sizeof(a));
    int64_t r2 = PlatfromGetRandom(b, sizeof(b));
    bool rng_ok = (r1 == 0 && r2 == 0 && memcmp(a, b, sizeof(a)) != 0);
    ESP_LOGI(TAG, "rng: %s (rc=%" PRId64 ")", rng_ok ? "OK" : "FAIL", r1);

    plat_timestamp_t t1 = 0, t2 = 0;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t1);
    PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, 10000000ULL, 0); /* 10 ms */
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t2);
    ESP_LOGI(TAG, "clock: %s (dt=%" PRIu64 " ns)",
             (t2 > t1) ? "OK" : "FAIL", (uint64_t)(t2 - t1));

    platform_mutex_t *m = PlatformMutexNew();
    if (m) {
        PlatformMutexLock(m);
        PlatformMutexUnlock(m);
        PlatformMutexFree(m);
    }
    ESP_LOGI(TAG, "mutex: %s", m ? "OK" : "FAIL");
}

void app_main(void)
{
    ESP_LOGI(TAG, "WANTED engine — ESP-IDF platform bring-up");
    selftest();
    ESP_LOGI(TAG, "selftest done");
}
