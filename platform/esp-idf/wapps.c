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

#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>
#include <wanted_malloc.h>

#include <debug_trace.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    pthread_t t;
    status_t status;
    wapp_data_t data;
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
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

/* Worker thread's native C stack. Set explicitly so every platform sizes it the
 * same way — the WAMR classic interpreter is recursive and the WASI/VFS host
 * calls add frames, and the RTOS per-thread default is far too small. Lives in
 * PSRAM (see startWorker). */
size_t PlatformWorkerStackSize(void) { return WASM_WORKER_STACK_SIZE; }

/* Base FreeRTOS priority, captured from the esp_pthread default. Wapps run at
 * this base; the supervisor one step above so it can always preempt and
 * terminate a runaway wapp. */
static int basePriority = -1;

/* Start a worker pthread for a wapp with its stack in PSRAM.
 * esp_pthread_set_cfg configures the next pthread_create on this thread; the
 * call is serialised by state_mtx. */
static int startWorker(pthread_t *t, wapp_data_t *data, int isSupervisor) {
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();

    if (basePriority < 0) {
        basePriority = (int)cfg.prio;
    }

    cfg.stack_size = WASM_WORKER_STACK_SIZE;
    cfg.prio = (size_t)(basePriority + (isSupervisor ? 1 : 0));
    cfg.thread_name = isSupervisor ? "wapp-super" : "wapp";
    /* Stack in PSRAM instead of internal DRAM. */
    cfg.stack_alloc_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    esp_pthread_set_cfg(&cfg);

    return pthread_create(t, NULL, WA_thread, (void *)data);
}

int PlatformWappStart(wapp_t *wapp) {
    int slot;

    if (NULL == wapp) {
        return -EINVAL;
    }

    pthread_mutex_lock(&state_mtx);
    if (state.n >= MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].status == NOT_STARTED ||
            state.threads[slot].status == EXITED ||
            state.threads[slot].status == FAILURE)
            break;
    }
    if (slot >= MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    /* The slot owns the previous occupant's wapp_t for its whole lifetime. Its
     * thread has fully terminated by the time the slot is reusable, so release
     * the image + struct now. The supervisor's image is a persistent singleton
     * reused across respawns — never free that one. */
    wapp_t *prev = state.threads[slot].data.wapp;
    if (prev != NULL && prev != wapp && prev != WantedGetCurrentSupervisor()) {
        PlatformWappUnload(prev);
        WantedFree(prev);
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

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if ((strcmp((char *)state.threads[slot].data.wapp->name, name) == 0) &&
            state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOENT;
    }

    /* Cooperative stop: set the terminate flag so wasm_runtime_call_wasm
     * returns false at the next instruction boundary. The thread unwinds
     * through WA_threadEnd. A worker blocked in a host call unwinds once that
     * call returns (no signal wakeup on ESP-IDF). */
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

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if (strcmp((char *)state.threads[slot].data.wapp->name, name) == 0)
            break;
    }

    if (slot == MAX_WAPPS) {
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

/* Consecutive supervisor launch FAILUREs tolerated before the engine stops
 * respawning. A clean supervisor exit is respawned indefinitely; a supervisor
 * that cannot launch (e.g. a malformed mount in its config) would otherwise
 * respawn-loop in silence, so bail loudly once it fails this many times. */
#define MAX_SUPERVISOR_LAUNCH_FAILURES 3

void PlatformWappLoop(void) {
    int supervisorFailures = 0;
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

        uint8_t supervisorOk = 0;
        int supervisorFailed = 0;
        int supervisorErr = 0;
        for (int i = 0; i < MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (state.threads[i].data.wapp == NULL)
                continue;
            if (strncmp((const char *)state.threads[i].data.wapp->name,
                        "supervisor", strlen("supervisor")) != 0)
                continue;
            if (state.threads[i].status == RUNNING) {
                supervisorOk++;
            } else if (state.threads[i].status == FAILURE) {
                supervisorFailed = 1;
                supervisorErr = state.threads[i].data.lastStatus;
            }
        }

        if (supervisorOk) {
            supervisorFailures = 0;
            /* Supervisor reached RUNNING at least once this boot: the image
             * is good. Confirms a PENDING_VERIFY slot and is a no-op once
             * already confirmed. */
            if (!otaConfirmed) {
                PlatformOtaConfirm();
                otaConfirmed = true;
            }
            continue;
        }

        /* No supervisor running. A clean exit is respawned (the supervisor is a
         * persistent singleton). A launch FAILURE that repeats is a fatal
         * misconfiguration, so stop loudly instead of respawning forever. */
        if (supervisorFailed &&
            ++supervisorFailures >= MAX_SUPERVISOR_LAUNCH_FAILURES) {
            fprintf(
                stderr,
                "wanted: supervisor failed to launch %d times in a row "
                "(error %d: %s); stopping — check the supervisor config\n",
                supervisorFailures, supervisorErr,
                strerror(supervisorErr < 0 ? -supervisorErr : supervisorErr));
            return;
        }
        if (!supervisorFailed)
            supervisorFailures = 0;
        PlatformWappStart(WantedGetCurrentSupervisor());
    }
}

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && (size_t)r < appsLen; i++) {
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
