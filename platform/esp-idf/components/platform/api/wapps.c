#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include <debug_trace.h>
#include <platform.h>
#include <wanted-api.h>

#define TAG "wapps"

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    TaskHandle_t t;
    status_t status;
    wapp_data_t data;
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} state;

/*
static void updateState(uint8_t id, int ret) {
    taskENTER_CRITICAL(&lock);
    if (ret == 0) {
        state.threads[id].status = EXITED;
    } else {
        state.threads[id].status = FAILURE;
    }
    state.n--;
    taskEXIT_CRITICAL(&lock);
}
*/

void WA_thread(void *params) {
    wapp_data_t *d = (wapp_data_t *)params;

    ESP_LOGE("wapps", "starting wa thread");

    if (d == NULL) {
        DEBUG_TRACE("parameters passed to thread are NULL");
        vTaskDelete(NULL);
    }

    taskENTER_CRITICAL(&lock);
    state.threads[d->id].status = RUNNING;
    taskEXIT_CRITICAL(&lock);

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    vTaskDelete(NULL);
}

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;
    size_t r;

    DEBUG_TRACE("Opening: %s\n", path);

    f = fopen(path, "rb");

    if (NULL == f) {
        FATAL(-errno, "can't open wapp: %s", path);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)malloc(filesize);
    if (img == NULL)
        FATAL(-errno, "can't malloc buffer");

    r = fread(img, 1, filesize, f);
    if (r < filesize) {
        FATAL(-errno, "can't read wapp image")
    }

    wapp->img = img;
    wapp->img_len = filesize;

    fclose(f);

    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    free(wapp->img);
    return 0;
}

int PlatformWappStart(wapp_t *app) {
    int slot;
    TaskHandle_t t;

    DEBUG_TRACE("Trying to start wapp...");
    ESP_LOGE(TAG, "free stack: %d", uxTaskGetStackHighWaterMark(NULL));

    taskENTER_CRITICAL(&lock);

    if (state.n >= MAX_WAPPS) {
        taskEXIT_CRITICAL(&lock);
        return -ENOSPC;
    }

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].status == NOT_STARTED ||
            state.threads[slot].status == EXITED ||
            state.threads[slot].status == FAILURE)
            break;
    }

    state.threads[slot].data.id = slot;
    state.threads[slot].data.wapp = *app;
    state.threads[slot].status = STARTING;
    taskEXIT_CRITICAL(&lock);

    xTaskCreatePinnedToCore(
        WA_thread, (const char *const)state.threads[slot].data.wapp.name, 65536,
        (void *)&state.threads[slot].data, tskIDLE_PRIORITY, &t, 1);
    configASSERT(t);

    taskENTER_CRITICAL(&lock);
    state.n++;
    state.threads[slot].t = t;
    taskEXIT_CRITICAL(&lock);

    DEBUG_TRACE("created task");
    ESP_LOGE(TAG, "free stack: %d", uxTaskGetStackHighWaterMark(NULL));
    return 0;
}

int PlatformWappStop(const char *name) {
    int slot;

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if ((strcmp((char *)state.threads[slot].data.wapp.name, name) == 0) &&
            state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == MAX_WAPPS) {
        return -ENOENT;
    }

    vTaskDelete(state.threads[slot].t);

    return 0;
}

void PlatformWappLoop() {
    uint8_t supervisorOk;
    DEBUG_TRACE("looping");
    ESP_LOGE(TAG, "free stack: %d", uxTaskGetStackHighWaterMark(NULL));

    for (;;) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        if (!state.n) {
            /* when only supervisor was running and it ended, let's exit */
            /* TODO: maybe this needs to be removed */
            return;
        }

        supervisorOk = 0;
        for (int i = 0; i < MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (strncmp((const char *)state.threads[i].data.wapp.name,
                        "supervisor", strlen("supervisor")) == 0 &&
                state.threads[i].status == RUNNING) {
                supervisorOk++;
            }
        }

        if (!supervisorOk) {
            PlatformWappStart(WantedGetCurrentSupervisor());
        }
    }
}

int PlatformWappGetState(wapp_state_t *apps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
        if (state.threads[i].data.wapp.img == NULL)
            continue;

        strncpy(apps[r].name, (const char *)state.threads[i].data.wapp.name,
                WAPP_MAX_NAME_LEN);
        apps[r].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        apps[r].status = state.threads[i].status;
        apps[r].version = state.threads[i].data.wapp.version;
        apps[r].id = state.threads[i].data.id;
        r++;
    }

    return r;
}
