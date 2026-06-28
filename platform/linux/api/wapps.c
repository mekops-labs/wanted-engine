/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <config-linux.h>
#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>
#include <wanted_malloc.h>

#include <debug_trace.h>

#ifdef __ANDROID__
#define GNU_SOURCE
#include <signal.h>
#endif

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

/* Consecutive supervisor launch FAILUREs tolerated before the engine aborts.
 * A clean supervisor exit is respawned indefinitely; a supervisor that cannot
 * launch (e.g. a malformed mount in its config) would otherwise respawn-loop in
 * silence, so bail loudly once it fails this many times in a row. */
#define MAX_SUPERVISOR_LAUNCH_FAILURES 3

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
 * PlatformWappLoop. argv is captured at startup so a reboot re-execs the same
 * binary. */
static int shutdown_requested = 0;
static int reboot_requested = 0;
static char **process_argv = NULL;

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

#ifdef __ANDROID__
static void thread_sigHandler(int sig) {
    int i;
    if (sig != SIGUSR1) {
        return;
    }

    pthread_t t = pthread_self();

    for (i = 0; i < MAX_WAPPS; i++) {
        if (state.threads[i].t == t)
            break;
    }
    if (i == MAX_WAPPS)
        return;

    WA_threadEnd((void *)&state.threads[i].data);

    pthread_exit(NULL);
}
#endif

void *WA_thread(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

#ifndef __ANDROID__
    /* Async cancellation is required: a wapp thread may be spinning in
     * interpreter code with no deferred cancellation point to reach. */
    /* NOLINTNEXTLINE(cert-pos47-c) */
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#else
    struct sigaction actions;
    memset(&actions, 0, sizeof(actions));
    sigemptyset(&actions.sa_mask);
    actions.sa_flags = 0;
    actions.sa_handler = thread_sigHandler;
    sigaction(SIGUSR1, &actions, NULL);
#endif

    pthread_cleanup_push(WA_threadEnd, ptr);

    pthread_mutex_lock(&state_mtx);
    state.threads[d->id].status = RUNNING;
    pthread_mutex_unlock(&state_mtx);

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    pthread_cleanup_pop(1);

    pthread_exit(NULL);
}

/* Init task's scheduling priority, captured on the first wapp start (which runs
 * in that task). Wapps run at this base; the supervisor one step above it. */
static int basePriority = -1;

/* Worker thread's native C stack. Set explicitly (not the 8 MB glibc default)
 * so every platform sizes it the same way — the WAMR classic interpreter is
 * recursive and the WASI/VFS host calls add frames (see WASM_WORKER_STACK_SIZE
 * in wanted-config.h). Floored at PTHREAD_STACK_MIN so a tight profile cannot
 * drop below what the C library accepts. */
static size_t worker_stacksize(void) {
    size_t ss = WASM_WORKER_STACK_SIZE;
#ifdef PTHREAD_STACK_MIN
    if (ss < (size_t)PTHREAD_STACK_MIN)
        ss = (size_t)PTHREAD_STACK_MIN;
#endif
    return ss;
}

size_t PlatformWorkerStackSize(void) { return worker_stacksize(); }

/* Start a worker thread for a wapp. The supervisor runs one scheduling step
 * above the wapps it manages so it can always preempt and terminate a runaway
 * (e.g. a never-yielding wapp). Priorities are set explicitly rather than
 * inherited: a wapp is launched from the supervisor's own (elevated) thread, so
 * inheriting would lift it to the supervisor's priority and defeat preemption.
 * A host that forbids real-time scheduling (Linux without CAP_SYS_NICE) returns
 * EPERM, so all threads fall back to default scheduling, where the host
 * scheduler time-slices regardless. */
static int startWorker(pthread_t *t, wapp_data_t *data, int isSupervisor) {
    pthread_attr_t attr;
    struct sched_param sp;
    int policy, hi, rc;

    if (basePriority < 0) {
        pthread_getschedparam(pthread_self(), &policy, &sp);
        basePriority = sp.sched_priority;
    }

    hi = sched_get_priority_max(SCHED_RR);
    sp.sched_priority = basePriority + (isSupervisor ? 1 : 0);
    if (hi > 0 && sp.sched_priority > hi)
        sp.sched_priority = hi;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, worker_stacksize());
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    rc = pthread_create(t, &attr, WA_thread, (void *)data);
    if (rc != 0) {
        /* No privilege for real-time scheduling: keep the explicit stack size
         * but inherit the parent's scheduling. */
        pthread_attr_setinheritsched(&attr, PTHREAD_INHERIT_SCHED);
        rc = pthread_create(t, &attr, WA_thread, (void *)data);
    }
    pthread_attr_destroy(&attr);
    return rc;
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

    /* The slot owns the previous occupant's wapp_t for its whole lifetime
     * (StartWapp hands ownership here, not freeing at the call site). Its
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

    startWorker((pthread_t *)&state.threads[slot].t,
                (wapp_data_t *)&state.threads[slot].data,
                wapp == WantedGetCurrentSupervisor());
    pthread_detach(state.threads[slot].t);
    state.n++;

    pthread_mutex_unlock(&state_mtx);

    return 0;
}

int PlatformWappStop(const char *name) {
    int slot;

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if ((strcmp((char *)state.threads[slot].data.wapp->name, name) == 0) &&
            state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == MAX_WAPPS) {
        return -ENOENT;
    }

#ifndef __ANDROID__
    return -pthread_cancel(state.threads[slot].t);
#else
    return -pthread_kill(state.threads[slot].t, SIGUSR1);
#endif
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
     * image here would be a use-after-free. The terminal status (set by
     * updateState after the worker unwound) guarantees the thread is gone. */
    if (state.threads[slot].status != EXITED &&
        state.threads[slot].status != FAILURE) {
        pthread_mutex_unlock(&state_mtx);
        return -EBUSY;
    }

    /* Free the mapped image + struct. The supervisor image is a persistent
     * singleton reused across respawns — never free that one. state.n was
     * already decremented when the worker reached its terminal status, so the
     * slot does not count against the pool and must not be decremented again.
     */
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

void PlatformSetProcessArgs(int argc, char **argv) {
    (void)argc;
    process_argv = argv;
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
    int supervisorFailures = 0;

    for (;;) {
        sleep(1);

        pthread_mutex_lock(&state_mtx);
        int shutdown = shutdown_requested;
        int reboot = reboot_requested;
        pthread_mutex_unlock(&state_mtx);

        if (shutdown) {
            /* Return so WantedStart and main unwind to a normal process exit.
             */
            return;
        }
        if (reboot) {
            /* Engine-level restart: re-exec the same binary. This is not a
             * system reboot — argv[0] is the original image captured at
             * startup. If exec fails, fall through to a clean exit. */
            if (process_argv && process_argv[0]) {
                execv(process_argv[0], process_argv);
                DEBUG_TRACE("reboot execv(%s) failed: %d", process_argv[0],
                            errno);
            }
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
                /* WantedWappRun's negative return — the launch error (e.g.
                 * -EINVAL for a malformed mount, -EROFS for a missing backing
                 * dir). */
                supervisorErr = state.threads[i].data.lastStatus;
            }
        }

        if (supervisorOk) {
            supervisorFailures = 0;
            continue;
        }

        /* No supervisor running. A clean exit is respawned (the supervisor is a
         * persistent singleton). A launch FAILURE that repeats is a fatal
         * misconfiguration — e.g. a malformed mount in the supervisor config —
         * so abort loudly instead of respawning in silence forever. */
        if (supervisorFailed &&
            ++supervisorFailures >= MAX_SUPERVISOR_LAUNCH_FAILURES) {
            fprintf(
                stderr,
                "wanted: supervisor failed to launch %d times in a row "
                "(error %d: %s); aborting — check the supervisor config\n",
                supervisorFailures, supervisorErr,
                strerror(supervisorErr < 0 ? -supervisorErr : supervisorErr));
            exit(EXIT_FAILURE);
        }
        if (!supervisorFailed)
            supervisorFailures = 0;
        PlatformWappStart(WantedGetCurrentSupervisor());
    }
}

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
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
        WantedWappMemStats(state.threads[i].data.wamr, &wapps[r]);
        r++;
    }

    return r;
}

#include <malloc.h>
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    struct mallinfo2 mi = mallinfo2();
    if (heap_used)
        *heap_used = mi.uordblks;
    if (heap_total)
        *heap_total = mi.arena;
}

const char *PlatformName(void) { return "linux"; }
