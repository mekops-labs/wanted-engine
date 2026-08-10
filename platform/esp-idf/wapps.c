/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform wapp lifecycle. */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_pthread.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "sdkconfig.h"

#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>
#include <wanted_malloc.h>

#include <debug_trace.h>
#include <wanted_log.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    pthread_t t;
    status_t status;
    wapp_data_t data;
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[CONFIG_WANTED_MAX_WAPPS];
} state;

/* System-control requests, raised by a privileged wapp and consumed by
 * PlatformWappLoop: shutdown deep-sleeps the SoC, reboot resets it. */
static int shutdown_requested = 0;
static int reboot_requested = 0;

static void updateState(uint8_t id, int ret) {
    pthread_mutex_lock(&state_mtx);
    if (ret == 0) {
        state.threads[id].status = EXITED;
    } else {
        state.threads[id].status = FAILURE;
    }
    state.n--;
    pthread_mutex_unlock(&state_mtx);
}

void WA_threadEnd(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

    WantedWappStop(d);

    updateState(d->id, d->lastStatus);
}

void *WA_thread(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

    pthread_mutex_lock(&state_mtx);
    state.threads[d->id].status = RUNNING;
    pthread_mutex_unlock(&state_mtx);

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    /* The stop path is cooperative (WantedWappTerminate aborts the in-flight
     * WASM call), so the worker always reaches here on a normal return — call
     * the teardown directly. */
    WA_threadEnd(d);

    pthread_exit(NULL);
    return NULL;
}

/* Worker thread's native C stack, set explicitly because the classic WAMR
 * interpreter is recursive and the RTOS per-thread default is far too small.
 * Lives in PSRAM; see startWorker. */
size_t PlatformWorkerStackSize(void) {
    return CONFIG_WANTED_WASM_WORKER_STACK_SIZE;
}

/* Base FreeRTOS priority, captured from the esp_pthread default. Wapps run at
 * this base; the supervisor one step above so it can always preempt and
 * terminate a runaway wapp. */
static int basePriority = -1;

/* Start a worker pthread for a wapp.
 * esp_pthread_set_cfg configures the next pthread_create on this thread; the
 * call is serialised by state_mtx. */
static int startWorker(pthread_t *t, wapp_data_t *data, int isSupervisor) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();

    if (basePriority < 0) {
        basePriority = (int)cfg.prio;
    }

    cfg.stack_size = CONFIG_WANTED_WASM_WORKER_STACK_SIZE;
    cfg.prio = (size_t)(basePriority + (isSupervisor ? 1 : 0));
    cfg.thread_name = isSupervisor ? "wapp-super" : "wapp";
#if CONFIG_IDF_TARGET_ESP32
    /* Worker threads read the flash-backed registry, and the classic part
     * disables its cache for a flash op, during which a PSRAM stack is
     * unreachable and asserts. This stack stays internal DRAM. */
#else
    /* Stack in PSRAM instead of internal DRAM. */
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
#endif
    esp_pthread_set_cfg(&cfg);

    return pthread_create(t, NULL, WA_thread, (void *)data);
}

int PlatformWappStart(wapp_t *wapp) {
    int slot;

    if (NULL == wapp) {
        return -EINVAL;
    }

    pthread_mutex_lock(&state_mtx);
    if (state.n >= CONFIG_WANTED_MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    /* A terminal slot still holds its wapp's name, status and exit code, and a
     * supervisor reads those after the run — `delete` is what releases one.
     * Taking it for the next start destroys that record, so only an empty slot
     * is free. A wapp restarting in place, the supervisor across a respawn,
     * keeps the slot it already holds. */
    for (slot = 0; slot < CONFIG_WANTED_MAX_WAPPS; slot++) {
        const wapp_t *occupant = state.threads[slot].data.wapp;
        if (occupant == NULL || occupant == wapp)
            break;
    }
    if (slot >= CONFIG_WANTED_MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    state.threads[slot].data.id = slot;
    state.threads[slot].data.wapp = wapp;
    state.threads[slot].data.exit_code = WAPP_EXIT_CODE_NONE;
    state.threads[slot].status = STARTING;

    int rc = startWorker((pthread_t *)&state.threads[slot].t,
                         (wapp_data_t *)&state.threads[slot].data,
                         wapp == WantedGetCurrentSupervisor());
    if (rc != 0) {
        state.threads[slot].status = FAILURE;
        state.threads[slot].data.wapp = NULL;
        pthread_mutex_unlock(&state_mtx);
        return -rc;
    }
    pthread_detach(state.threads[slot].t);
    state.n++;

    pthread_mutex_unlock(&state_mtx);

    return 0;
}

int PlatformWappStop(const char *name) {
    int slot;

    /* Hold state_mtx across the match and terminate so the worker cannot be
     * reaped (status flipped, slot reused) out from under us. */
    pthread_mutex_lock(&state_mtx);

    for (slot = 0; slot < CONFIG_WANTED_MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if ((strcmp((char *)state.threads[slot].data.wapp->name, name) == 0) &&
            state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == CONFIG_WANTED_MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOENT;
    }

    /* Cooperative stop: set the terminate flag so wasm_runtime_call_wasm
     * returns false at the next instruction boundary and the thread unwinds
     * through WA_threadEnd. ESP-IDF wires no signal wakeup. */
    WantedWappTerminate((wapp_data_t *)&state.threads[slot].data);

    pthread_mutex_unlock(&state_mtx);

    return 0;
}

int PlatformWappRelease(const char *name) {
    int slot;

    if (NULL == name) {
        return -EINVAL;
    }

    pthread_mutex_lock(&state_mtx);

    for (slot = 0; slot < CONFIG_WANTED_MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if (strcmp((char *)state.threads[slot].data.wapp->name, name) == 0)
            break;
    }

    if (slot == CONFIG_WANTED_MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOENT;
    }

    /* Only a terminal slot can be released. A running/starting wapp must be
     * stopped first — its worker still dereferences the slot, so freeing the
     * image here would be a use-after-free. */
    if (state.threads[slot].status != EXITED &&
        state.threads[slot].status != FAILURE) {
        pthread_mutex_unlock(&state_mtx);
        return -EBUSY;
    }

    /* Free the mapped image + struct. The supervisor image is a persistent
     * singleton reused across respawns — never free that one. state.n was
     * already decremented when the worker reached its terminal status. */
    wapp_t *w = state.threads[slot].data.wapp;
    if (w != NULL && w != WantedGetCurrentSupervisor()) {
        PlatformWappUnload(w);
        WantedFree(w);
    }
    state.threads[slot].data.wapp = NULL;
    state.threads[slot].data.id = 0;
    state.threads[slot].data.exit_code = WAPP_EXIT_CODE_NONE;
    state.threads[slot].status = NOT_STARTED;

    pthread_mutex_unlock(&state_mtx);
    return 0;
}

/* A board reset replaces the whole image, so the captured argv is unused. Kept
 * for platform-API symmetry with the host. */
void PlatformSetProcessArgs(int argc, char **argv) {
    (void)argc;
    (void)argv;
}

void PlatformRequestShutdown(void) {
    pthread_mutex_lock(&state_mtx);
    shutdown_requested = 1;
    pthread_mutex_unlock(&state_mtx);
}

void PlatformRequestReboot(void) {
    pthread_mutex_lock(&state_mtx);
    reboot_requested = 1;
    pthread_mutex_unlock(&state_mtx);
}

void PlatformWappLoop(void) {
    bool otaConfirmed = false;

    for (;;) {
        sleep(1);

        pthread_mutex_lock(&state_mtx);
        int shutdown = shutdown_requested;
        int reboot = reboot_requested;
        pthread_mutex_unlock(&state_mtx);

        if (shutdown) {
            esp_deep_sleep_start();
            return;
        }
        if (reboot) {
            esp_restart();
            return;
        }

        bool supervisorOk = false;
        bool supervisorFailed = false;
        bool supervisorExited = false;
        int supervisorErr = 0;
        for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (state.threads[i].data.wapp == NULL)
                continue;
            if (strncmp((const char *)state.threads[i].data.wapp->name,
                        "supervisor", strlen("supervisor")) != 0)
                continue;
            if (state.threads[i].status == RUNNING) {
                supervisorOk = true;
            } else if (state.threads[i].status == EXITED) {
                supervisorExited = true;
            } else if (state.threads[i].status == FAILURE) {
                supervisorFailed = true;
                supervisorErr = state.threads[i].data.lastStatus;
            }
        }

        switch (WantedSupervisorObserve(supervisorOk, supervisorFailed,
                                        supervisorExited)) {
        case SUPERVISOR_HEALTHY:
            /* Supervisor reached RUNNING at least once this boot: the image
             * is good. Confirms a PENDING_VERIFY slot and is a no-op once
             * already confirmed. */
            if (!otaConfirmed) {
                PlatformOtaConfirm();
                otaConfirmed = true;
            }
            continue;
        case SUPERVISOR_RESPAWN:
            break;
        case SUPERVISOR_ROLLED_BACK:
            fprintf(stderr,
                    "wanted: staged supervisor failed %d times in a row (%s); "
                    "falling back to the built-in image\n",
                    MAX_SUPERVISOR_LAUNCH_FAILURES,
                    supervisorFailText(supervisorFailed, supervisorErr));
            break;
        case SUPERVISOR_UNRECOVERABLE:
            fprintf(stderr,
                    "wanted: supervisor failed %d times in a row (%s); "
                    "stopping — check the supervisor config\n",
                    MAX_SUPERVISOR_LAUNCH_FAILURES,
                    supervisorFailText(supervisorFailed, supervisorErr));
            return;
        }
        PlatformWappStart(WantedGetCurrentSupervisor());
    }
}

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < CONFIG_WANTED_MAX_WAPPS && (size_t)r < appsLen;
         i++) {
        if (state.threads[i].data.wapp == NULL)
            continue;

        strncpy(wapps[r].name, (const char *)state.threads[i].data.wapp->name,
                WAPP_MAX_NAME_LEN);
        wapps[r].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        strncpy(wapps[r].image, (const char *)state.threads[i].data.wapp->image,
                WAPP_MAX_NAME_LEN);
        wapps[r].image[WAPP_MAX_NAME_LEN - 1] = '\0';
        wapps[r].status = state.threads[i].status;
        strncpy(wapps[r].version,
                (const char *)state.threads[i].data.wapp->version,
                WAPP_MAX_VERSION_LEN);
        wapps[r].version[WAPP_MAX_VERSION_LEN - 1] = '\0';
        wapps[r].id = state.threads[i].data.id;
        wapps[r].exit_code = state.threads[i].data.exit_code;
        /* Only a running wapp has a live WAMR instance to sample. */
        WantedWappMemStats(
            wapps[r].status == RUNNING ? state.threads[i].data.wamr : NULL,
            &wapps[r]);
        r++;
    }

    return r;
}
