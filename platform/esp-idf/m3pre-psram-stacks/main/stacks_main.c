/*
 * ESP32-S3 PSRAM-resident task-stack safety experiment (M3pre).
 *
 * Validates the precondition for Path B of the ESP-IDF wapp lifecycle: placing
 * worker task stacks in PSRAM instead of internal RAM, so the wapp count is not
 * bounded by MAX_WAPPS * WASM_WORKER_STACK_SIZE of scarce internal DRAM.
 *
 * Several worker tasks are created with xTaskCreateStaticPinnedToCore, each
 * given a stack buffer allocated from octal PSRAM (heap_caps_malloc
 * MALLOC_CAP_SPIRAM); their TCBs stay in internal RAM. Each worker repeatedly
 * fills and verifies a large on-stack canary buffer and recurses to touch deep
 * stack, yielding to force context switches. Concurrently a flash-thrash task
 * erases, writes, reads and byte-verifies a raw flash partition — the
 * operations that bracket a cache disable, which SPI_FLASH_AUTO_SUSPEND keeps
 * the cache live through on the S3.
 *
 * A PSRAM-stack that loses coherence across a flash cache-disable window shows
 * up as a canary mismatch (or a crash if the fault is fatal); flash corruption
 * shows up as a read/verify mismatch. The build self-reports which mitigations
 * are compiled in, so a captured log is self-identifying. Prints VERDICT: CLEAN
 * / CORRUPTION.
 */

#include <inttypes.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TAG "m3pre"

#define PATTERN_PART_LABEL "pattern"
#define PATTERN_PART_SUBTYPE 0x40u

#define N_WORKERS 8
#define WORKER_STACK_BYTES (16u * 1024u) /* PSRAM stack per worker */
#define ONSTACK_CANARY_BYTES 4096u       /* large on-stack buffer, exercised */
#define RECURSE_DEPTH 6                  /* touch deep stack each iteration */
#define RECURSE_FRAME_BYTES 256u

#define ERASE_WINDOW                                                           \
    (64u * 1024u)        /* per flash cycle; multiple of 4 KiB sector */
#define FLASH_CHUNK 256u /* divides ERASE_WINDOW evenly */
#define FLASH_CORE 0

#define RUN_SECONDS 30
#define PROGRESS_SECONDS 5

/* Deterministic byte for a given flash offset — hashed so mismatches are
 * unambiguous and not trivially cache-predictable. */
static inline uint8_t pattern_byte(uint32_t off) {
    uint32_t x = off * 2654435761u;
    x ^= x >> 15;
    return (uint8_t)x;
}

static const esp_partition_t *g_part;

static atomic_uint_least64_t flash_cycles;
static atomic_uint_least64_t flash_read_errs;
static atomic_uint_least64_t flash_mismatch;
static atomic_uint_least64_t worker_iters;
static atomic_uint_least64_t stack_mismatch;
static atomic_int workers_done;
static atomic_bool flash_done;
static atomic_bool stop_flag;

/* Recurse to consume real stack, writing and reading back a per-frame marker so
 * the compiler cannot elide the frames. Runs on the worker's PSRAM stack. */
static void stack_recurse(int depth, uint8_t tag, volatile uint8_t *sink) {
    volatile uint8_t frame[RECURSE_FRAME_BYTES];
    for (uint32_t i = 0; i < RECURSE_FRAME_BYTES; i++) {
        frame[i] = (uint8_t)(tag + depth + i);
    }
    if (depth > 0) {
        stack_recurse(depth - 1, tag, sink);
    }
    uint8_t acc = 0;
    for (uint32_t i = 0; i < RECURSE_FRAME_BYTES; i++) {
        acc = (uint8_t)(acc ^ frame[i]);
    }
    *sink = acc;
}

static void worker_task(void *arg) {
    int id = (int)(intptr_t)arg;
    uint8_t tag = (uint8_t)(0xA0u + (unsigned)id);

    while (!atomic_load(&stop_flag)) {
        /* Large live buffer on the (PSRAM) stack. Written, then re-read after a
         * forced context switch — the interval during which a concurrent flash
         * op may disable the cache. */
        uint8_t canary[ONSTACK_CANARY_BYTES];
        for (uint32_t i = 0; i < ONSTACK_CANARY_BYTES; i++) {
            canary[i] = (uint8_t)(tag + (i >> 3));
        }

        volatile uint8_t sink = 0;
        stack_recurse(RECURSE_DEPTH, tag, &sink);

        vTaskDelay(
            1); /* yield so a flash op can run while this stack is live */

        uint64_t bad = 0;
        for (uint32_t i = 0; i < ONSTACK_CANARY_BYTES; i++) {
            if (canary[i] != (uint8_t)(tag + (i >> 3))) {
                bad++;
            }
        }
        if (bad) {
            atomic_fetch_add(&stack_mismatch, bad);
        }
        atomic_fetch_add(&worker_iters, 1);
    }

    atomic_fetch_add(&workers_done, 1);
    vTaskDelete(NULL);
}

/* Erase / write / read+verify a rolling window of the raw flash partition until
 * the deadline. Erase and program are the longest cache-disable brackets. */
static void flash_thrash_task(void *arg) {
    (void)arg;
    uint8_t buf[FLASH_CHUNK];
    uint32_t win = 0;
    const uint32_t nwin = (uint32_t)(g_part->size / ERASE_WINDOW);

    while (!atomic_load(&stop_flag)) {
        uint32_t base = win * ERASE_WINDOW;
        win = (win + 1u) % nwin;

        esp_err_t err = esp_partition_erase_range(g_part, base, ERASE_WINDOW);
        if (err != ESP_OK) {
            atomic_fetch_add(&flash_read_errs, 1);
            continue;
        }
        for (uint32_t off = 0; off < ERASE_WINDOW; off += FLASH_CHUNK) {
            for (uint32_t i = 0; i < FLASH_CHUNK; i++) {
                buf[i] = pattern_byte(base + off + i);
            }
            err = esp_partition_write(g_part, base + off, buf, FLASH_CHUNK);
            if (err != ESP_OK) {
                atomic_fetch_add(&flash_read_errs, 1);
            }
        }
        for (uint32_t off = 0; off < ERASE_WINDOW; off += FLASH_CHUNK) {
            err = esp_partition_read(g_part, base + off, buf, FLASH_CHUNK);
            if (err != ESP_OK) {
                atomic_fetch_add(&flash_read_errs, 1);
                continue;
            }
            for (uint32_t i = 0; i < FLASH_CHUNK; i++) {
                if (buf[i] != pattern_byte(base + off + i)) {
                    atomic_fetch_add(&flash_mismatch, 1);
                    break;
                }
            }
        }
        atomic_fetch_add(&flash_cycles, 1);
    }

    atomic_store(&flash_done, true);
    vTaskDelete(NULL);
}

static int spawn_worker(int id) {
    /* Stack in PSRAM; TCB in internal RAM (the scheduler touches the TCB from
     * contexts where the external-RAM cache may be disabled). */
    StackType_t *stack = heap_caps_malloc(WORKER_STACK_BYTES,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t *tcb = heap_caps_malloc(sizeof(StaticTask_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (stack == NULL || tcb == NULL) {
        ESP_LOGE(TAG, "worker %d alloc failed (stack=%p tcb=%p)", id,
                 (void *)stack, (void *)tcb);
        return -1;
    }
    if (!esp_ptr_external_ram(stack)) {
        ESP_LOGE(TAG,
                 "worker %d stack %p is NOT in PSRAM — aborting experiment", id,
                 (void *)stack);
        return -1;
    }

    char name[16];
    snprintf(name, sizeof(name), "w%d", id);
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(
        worker_task, name, WORKER_STACK_BYTES, (void *)(intptr_t)id, 5, stack,
        tcb, id % portNUM_PROCESSORS);
    if (h == NULL) {
        ESP_LOGE(TAG, "worker %d create failed", id);
        return -1;
    }
    ESP_LOGI(TAG, "worker %d: PSRAM stack @%p (%u B) core %d", id,
             (void *)stack, (unsigned)WORKER_STACK_BYTES,
             id % portNUM_PROCESSORS);
    return 0;
}

void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-S3 PSRAM task-stack safety experiment ===");
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

    g_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)PATTERN_PART_SUBTYPE,
        PATTERN_PART_LABEL);
    if (g_part == NULL) {
        ESP_LOGE(TAG, "pattern partition not found");
        return;
    }
    ESP_LOGI(TAG, "pattern partition @0x%08" PRIx32 " size %" PRIu32,
             g_part->address, g_part->size);

    for (int i = 0; i < N_WORKERS; i++) {
        if (spawn_worker(i) != 0) {
            return;
        }
    }

    xTaskCreatePinnedToCore(flash_thrash_task, "flash", 4096, NULL, 5, NULL,
                            FLASH_CORE);

    ESP_LOGI(TAG, "%d PSRAM-stacked workers + flash thrash; running %d s ...",
             N_WORKERS, RUN_SECONDS);

    int64_t t_end = esp_timer_get_time() + (int64_t)RUN_SECONDS * 1000000;
    int64_t t_next = esp_timer_get_time() + (int64_t)PROGRESS_SECONDS * 1000000;
    while (esp_timer_get_time() < t_end) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (esp_timer_get_time() >= t_next) {
            ESP_LOGI(TAG,
                     "progress: flash_cycles=%" PRIu64 " read_err=%" PRIu64
                     " flash_mismatch=%" PRIu64 " | worker_iters=%" PRIu64
                     " stack_mismatch=%" PRIu64,
                     atomic_load(&flash_cycles), atomic_load(&flash_read_errs),
                     atomic_load(&flash_mismatch), atomic_load(&worker_iters),
                     atomic_load(&stack_mismatch));
            t_next += (int64_t)PROGRESS_SECONDS * 1000000;
        }
    }

    atomic_store(&stop_flag, true);
    while (atomic_load(&workers_done) < N_WORKERS ||
           !atomic_load(&flash_done)) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    uint64_t fc = atomic_load(&flash_cycles);
    uint64_t fre = atomic_load(&flash_read_errs);
    uint64_t fm = atomic_load(&flash_mismatch);
    uint64_t wi = atomic_load(&worker_iters);
    uint64_t sm = atomic_load(&stack_mismatch);

    ESP_LOGI(TAG, "=== RESULT ===");
    ESP_LOGI(TAG,
             "flash_cycles=%" PRIu64 " read_err=%" PRIu64
             " flash_mismatch=%" PRIu64,
             fc, fre, fm);
    ESP_LOGI(TAG, "worker_iters=%" PRIu64 " stack_mismatch=%" PRIu64, wi, sm);
    if (fre == 0 && fm == 0 && sm == 0) {
        ESP_LOGI(TAG, "VERDICT: CLEAN");
    } else {
        ESP_LOGE(TAG, "VERDICT: CORRUPTION");
    }
    ESP_LOGI(TAG, "=== DONE ===");
}
