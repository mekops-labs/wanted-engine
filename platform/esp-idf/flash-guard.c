/* SPDX-License-Identifier: Apache-2.0 */

/* With code in PSRAM and no flash auto-suspend, a flash write leaves the cache
 * on and a read through a mapping returns erased bytes. The flash start and end
 * hooks take a lock that a reader of a mapping also takes. */

#include <pthread.h>
#include <stdbool.h>

#include <platform.h>

#include "esp_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

#if CONFIG_SPIRAM_FETCH_INSTRUCTIONS && CONFIG_SPIRAM_RODATA &&                \
    !CONFIG_SPI_FLASH_AUTO_SUSPEND
#define GUARD_NEEDED 1
#endif

#ifdef GUARD_NEEDED

static SemaphoreHandle_t g_lock;
static esp_flash_os_functions_t g_os;
static const esp_flash_os_functions_t *g_orig;
static bool g_ready;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static bool schedulerRunning(void) {
    return xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
           !xPortInIsrContext();
}

static esp_err_t guardStart(void *arg) {
    bool locked = schedulerRunning();
    esp_err_t rc;

    if (locked)
        xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
    rc = g_orig->start(arg);
    if (rc != ESP_OK && locked)
        xSemaphoreGiveRecursive(g_lock);
    return rc;
}

static esp_err_t guardEnd(void *arg) {
    esp_err_t rc = g_orig->end(arg);

    if (schedulerRunning() &&
        xSemaphoreGetMutexHolder(g_lock) == xTaskGetCurrentTaskHandle())
        xSemaphoreGiveRecursive(g_lock);
    return rc;
}

static void install(void) {
    esp_flash_t *chip = esp_flash_default_chip;

    g_lock = xSemaphoreCreateRecursiveMutex();
    if (g_lock == NULL || chip == NULL || chip->os_func == NULL)
        return;
    g_orig = chip->os_func;
    g_os = *g_orig;
    g_os.start = guardStart;
    g_os.end = guardEnd;
    chip->os_func = &g_os;
    g_ready = true;
}

void PlatformFlashReadBegin(void) {
    pthread_once(&g_once, install);
    if (g_ready)
        xSemaphoreTakeRecursive(g_lock, portMAX_DELAY);
}

void PlatformFlashReadEnd(void) {
    if (g_ready)
        xSemaphoreGiveRecursive(g_lock);
}

#else

void PlatformFlashReadBegin(void) {}
void PlatformFlashReadEnd(void) {}

#endif
