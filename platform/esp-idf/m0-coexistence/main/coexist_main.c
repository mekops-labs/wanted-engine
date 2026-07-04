/*
 * ESP32-S3 flash / PSRAM coexistence reproducer.
 *
 * Drives the hazardous pattern directly: one thread hammers a large PSRAM
 * working set (defeating the data cache) on the APP core, while another thread
 * reads a known pattern back from raw flash on the PRO core and byte-verifies
 * it. A flash read goes through the SPI-flash driver, which brackets the SPI1
 * transfer with a global cache disable unless CONFIG_SPI_FLASH_AUTO_SUSPEND
 * keeps it enabled; during that window a concurrent PSRAM access (or, under
 * CONFIG_SPIRAM_XIP_FROM_PSRAM, code fetch) can observe corruption.
 *
 * The build self-reports which mitigations are compiled in, so the two config
 * variants are distinguishable from the boot log alone.
 */

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "m0"
#define PATTERN_PART_LABEL "pattern"
#define PATTERN_PART_SUBTYPE 0x40u
#define PATTERN_BYTES (512u * 1024u) /* multiple of the 4 KiB erase sector */
#define FLASH_CHUNK 256u             /* divides PATTERN_BYTES evenly */
#define PSRAM_BYTES (2u * 1024u * 1024u)
#define PSRAM_STRIDE 4099u /* prime > cache line, defeats locality */
#define PSRAM_CORE 1
#define RUN_SECONDS 30
#define PROGRESS_SECONDS 5

/* Deterministic byte for a given flash offset — hashed so it is not trivially
 * cache-predictable and mismatches are unambiguous. */
static inline uint8_t pattern_byte(uint32_t off) {
    uint32_t x = off * 2654435761u;
    x ^= x >> 15;
    return (uint8_t)x;
}

static const esp_partition_t *g_part;

static atomic_uint_least64_t flash_reads;
static atomic_uint_least64_t flash_read_errs;
static atomic_uint_least64_t flash_mismatch;
static atomic_uint_least64_t psram_iters;
static atomic_uint_least64_t psram_mismatch;
static atomic_bool stop_flag;

/* Fill the pattern partition once, single-threaded, before the churn starts. */
static int flash_prepare(void) {
    g_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)PATTERN_PART_SUBTYPE,
        PATTERN_PART_LABEL);
    if (g_part == NULL) {
        ESP_LOGE(TAG, "pattern partition not found");
        return -1;
    }
    ESP_LOGI(TAG, "pattern partition @0x%08" PRIx32 " size %" PRIu32,
             g_part->address, g_part->size);
    if (PATTERN_BYTES > g_part->size) {
        ESP_LOGE(TAG, "pattern partition too small");
        return -1;
    }

    esp_err_t err = esp_partition_erase_range(g_part, 0, PATTERN_BYTES);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: %s", esp_err_to_name(err));
        return -1;
    }

    uint8_t buf[FLASH_CHUNK];
    for (uint32_t off = 0; off < PATTERN_BYTES; off += FLASH_CHUNK) {
        for (uint32_t i = 0; i < FLASH_CHUNK; i++) {
            buf[i] = pattern_byte(off + i);
        }
        err = esp_partition_write(g_part, off, buf, FLASH_CHUNK);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "write @%" PRIu32 ": %s", off, esp_err_to_name(err));
            return -1;
        }
    }
    ESP_LOGI(TAG, "pattern written (%u bytes)", (unsigned)PATTERN_BYTES);
    return 0;
}

static void *psram_thread(void *arg) {
    (void)arg;
    uint8_t *p =
        heap_caps_malloc(PSRAM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == NULL) {
        ESP_LOGE(TAG, "PSRAM alloc failed");
        return NULL;
    }
    ESP_LOGI(TAG, "PSRAM buffer @%p (%u bytes) on core %d", p,
             (unsigned)PSRAM_BYTES, xPortGetCoreID());

    uint32_t seed = 1;
    while (!atomic_load(&stop_flag)) {
        uint8_t base = (uint8_t)seed;
        for (uint32_t i = 0; i < PSRAM_BYTES; i += PSRAM_STRIDE) {
            p[i] = (uint8_t)(base + (i >> 8));
        }
        for (uint32_t i = 0; i < PSRAM_BYTES; i += PSRAM_STRIDE) {
            if (p[i] != (uint8_t)(base + (i >> 8))) {
                atomic_fetch_add(&psram_mismatch, 1);
            }
        }
        seed++;
        atomic_fetch_add(&psram_iters, 1);
    }
    heap_caps_free(p);
    return NULL;
}

/* Read + verify the whole pattern partition on the calling (PRO) core until the
 * deadline; prints a progress line every PROGRESS_SECONDS. */
static void flash_loop(void) {
    uint8_t buf[FLASH_CHUNK];
    int64_t t_end = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;
    int64_t t_next = esp_timer_get_time() + (int64_t)PROGRESS_SECONDS * 1000000;

    while (esp_timer_get_time() < t_end) {
        for (uint32_t off = 0; off < PATTERN_BYTES; off += FLASH_CHUNK) {
            esp_err_t err = esp_partition_read(g_part, off, buf, FLASH_CHUNK);
            atomic_fetch_add(&flash_reads, 1);
            if (err != ESP_OK) {
                atomic_fetch_add(&flash_read_errs, 1);
                continue;
            }
            for (uint32_t i = 0; i < FLASH_CHUNK; i++) {
                if (buf[i] != pattern_byte(off + i)) {
                    atomic_fetch_add(&flash_mismatch, 1);
                    break;
                }
            }
        }
        if (esp_timer_get_time() >= t_next) {
            ESP_LOGI(TAG,
                     "progress: flash_reads=%" PRIu64 " read_err=%" PRIu64
                     " mismatch=%" PRIu64 " | psram_iters=%" PRIu64
                     " psram_mismatch=%" PRIu64,
                     atomic_load(&flash_reads), atomic_load(&flash_read_errs),
                     atomic_load(&flash_mismatch), atomic_load(&psram_iters),
                     atomic_load(&psram_mismatch));
            t_next += (int64_t)PROGRESS_SECONDS * 1000000;
        }
    }
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 flash/PSRAM coexistence reproducer ===");
#if CONFIG_SPIRAM_XIP_FROM_PSRAM
    ESP_LOGI(TAG, "config: SPIRAM_XIP_FROM_PSRAM = ON");
#else
    ESP_LOGI(TAG, "config: SPIRAM_XIP_FROM_PSRAM = OFF");
#endif
#if CONFIG_SPI_FLASH_AUTO_SUSPEND
    ESP_LOGI(TAG, "config: SPI_FLASH_AUTO_SUSPEND = ON");
#else
    ESP_LOGI(TAG, "config: SPI_FLASH_AUTO_SUSPEND = OFF");
#endif

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM total: %u bytes", (unsigned)psram_total);
    if (psram_total == 0) {
        ESP_LOGE(TAG, "no PSRAM detected — aborting");
        return;
    }

    if (flash_prepare() != 0) {
        return;
    }

    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = 8192;
    cfg.pin_to_core = PSRAM_CORE;
    cfg.prio = 5;
    esp_pthread_set_cfg(&cfg);

    pthread_t th;
    if (pthread_create(&th, NULL, psram_thread, NULL) != 0) {
        ESP_LOGE(TAG, "pthread_create failed");
        return;
    }

    ESP_LOGI(TAG, "running %d s (flash reader on core %d) ...", RUN_SECONDS,
             xPortGetCoreID());
    flash_loop();

    atomic_store(&stop_flag, true);
    pthread_join(th, NULL);

    uint64_t fr = atomic_load(&flash_reads);
    uint64_t fre = atomic_load(&flash_read_errs);
    uint64_t fm = atomic_load(&flash_mismatch);
    uint64_t pi = atomic_load(&psram_iters);
    uint64_t pm = atomic_load(&psram_mismatch);

    ESP_LOGI(TAG, "=== RESULT ===");
    ESP_LOGI(TAG,
             "flash_reads=%" PRIu64 " read_err=%" PRIu64 " mismatch=%" PRIu64,
             fr, fre, fm);
    ESP_LOGI(TAG, "psram_iters=%" PRIu64 " psram_mismatch=%" PRIu64, pi, pm);
    if (fre == 0 && fm == 0 && pm == 0) {
        ESP_LOGI(TAG, "VERDICT: CLEAN");
    } else {
        ESP_LOGE(TAG, "VERDICT: CORRUPTION");
    }
    ESP_LOGI(TAG, "=== DONE ===");
}
