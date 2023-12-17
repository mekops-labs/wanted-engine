#include <stddef.h>
#include <string.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <wanted-api.h>

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

int PlatformWappLoad(const char *name, wapp_t * wapp)
{
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp)
{
    return 0;
}

int PlatformWappStart(wapp_t app)
{
    int slot;

    taskENTER_CRITICAL(&lock);

    if (state.n >= MAX_WAPPS) {
        taskEXIT_CRITICAL(&lock);
        return -ENOSPC;
    }

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].status == NOT_STARTED || state.threads[slot].status == EXITED || state.threads[slot].status == FAILURE)
            break;
    }

    state.threads[slot].data.id = slot;
    state.threads[slot].data.wapp = app;
    state.threads[slot].status = STARTING;

    taskEXIT_CRITICAL(&lock);
    return 0;
}

int PlatformWappStop(const char* name)
{
    int slot;

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if ((strcmp((char *)state.threads[slot].data.wapp.name, name) == 0)
            && state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == MAX_WAPPS) {
        return -ENOENT;
    }

    vTaskDelete(state.threads[slot].t);

    return 0;
}

void PlatformWappLoop()
{
    uint8_t supervisorOk;

    for (;;) {
        //sleep(1);

        if (!state.n) {
            /* when only supervisor was running and it ended, let's exit */
            /* TODO: maybe this needs to be removed */
            return;
        }

        supervisorOk = 0;
        for (int i = 0; i < MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (strncmp((const char*)state.threads[i].data.wapp.name, "supervisor", strlen("supervisor")) == 0 &&
                state.threads[i].status == RUNNING) {
                supervisorOk++;
            }
        }

        if (!supervisorOk) {
            PlatformWappStart(WantedGetCurrentSupervisor());
        }
    }
}

int PlatformWappGetState(wapp_state_t *apps, size_t appsLen)
{
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
        if (state.threads[i].data.wapp.img == NULL) continue;

        strncpy(apps[r].name, (const char *)state.threads[i].data.wapp.name, WAPP_MAX_NAME_LEN);
        apps[r].name[WAPP_MAX_NAME_LEN-1] = '\0';
        apps[r].status = state.threads[i].status;
        apps[r].version = state.threads[i].data.wapp.version;
        apps[r].id = state.threads[i].data.id;
        r++;
    }

    return r;
}
