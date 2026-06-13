/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX platform wapp lifecycle.
 *
 * Threading mirrors the Linux platform (detached pthreads, one slot per wapp),
 * but the stop path differs: instead of pthread_cancel — unreliable on NuttX —
 * a stopped wapp is terminated cooperatively. PlatformWappStop sets the
 * per-instruction terminate flag (WantedWappTerminate) and sends the worker a
 * signal (WAPP_STOP_SIGNAL) to wake any host call it is blocked in. The handler
 * records the interrupt on the worker's slot; PlatformClockNanoSleep turns that
 * into EINTR (NuttX wakes the sleep but reports success, so the timer return
 * cannot signal it alone), which unwinds the host call — including the pipe
 * driver's poll loop — back to the interpreter. The interpreter then sees the
 * terminate flag, aborts the in-flight WASM call, and the worker unwinds via
 * WA_threadEnd. This makes a wapp parked indefinitely in I/O (a read on an empty
 * pipe, a long sleep) promptly terminable — not just one executing WASM. */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __NuttX__
#include <sys/boardctl.h>
#endif

#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>
#include <wanted_malloc.h>
#include <wapp-stop.h>

#include <debug_trace.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Signal used to interrupt a worker blocked in a host syscall so the cooperative
 * stop can take effect. SIGUSR2, because WAMR reserves SIGUSR1 for its own
 * blocking-op wakeup and keeps it masked on every wasm thread, so a SIGUSR1 sent
 * here would never be delivered to the worker. */
#define WAPP_STOP_SIGNAL SIGUSR2

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

typedef struct {
    pthread_t t;
    status_t status;
    wapp_data_t data;
    sig_atomic_t interrupted; /* set by the stop handler on this worker */
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} state;

/* System-control requests, raised by a privileged wapp and consumed by
 * PlatformWappLoop: shutdown powers the board off, reboot resets it. */
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

/* Stop signal handler. Delivery wakes the worker's in-flight blocking host call
 * early; it records the interrupt on this worker's slot so PlatformClockNanoSleep
 * can report EINTR (NuttX reports the woken sleep as success, so the timer return
 * cannot signal it). The thread then unwinds normally through WA_threadEnd,
 * honouring the terminate flag — we deliberately do NOT pthread_exit from here
 * (fragile with live WAMR state) nor pthread_cancel (unreliable on NuttX). */
static void stopSigHandler(int sig) {
    (void)sig;
    pthread_t self = pthread_self();
    for (int i = 0; i < MAX_WAPPS; i++) {
        if (pthread_equal(state.threads[i].t, self)) {
            state.threads[i].interrupted = 1;
            break;
        }
    }
}

/* Read and clear the calling worker's pending stop interrupt (see wapp-stop.h). */
bool PlatformStopInterruptConsume(void) {
    pthread_t self = pthread_self();
    for (int i = 0; i < MAX_WAPPS; i++) {
        if (pthread_equal(state.threads[i].t, self)) {
            if (state.threads[i].interrupted) {
                state.threads[i].interrupted = 0;
                return true;
            }
            return false;
        }
    }
    return false;
}

void *WA_thread(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

    /* Install the stop handler (SA_RESTART cleared) and unblock the stop signal
     * on this worker so PlatformWappStop's pthread_kill can interrupt a blocked
     * host call. The disposition is process-wide and idempotent; the mask is
     * per-thread. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART → the blocked call returns, not restarts */
    sa.sa_handler = stopSigHandler;
    sigaction(WAPP_STOP_SIGNAL, &sa, NULL);

    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, WAPP_STOP_SIGNAL);
    pthread_sigmask(SIG_UNBLOCK, &unblock, NULL);

    pthread_mutex_lock(&state_mtx);
    state.threads[d->id].status = RUNNING;
    pthread_mutex_unlock(&state_mtx);

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    /* The stop path is cooperative (WantedWappTerminate aborts the in-flight
     * WASM call), never pthread_cancel, so the worker always reaches here on a
     * normal return — call the teardown directly. A pthread_cleanup handler
     * would not run anyway: it is a no-op unless CONFIG_PTHREAD_CLEANUP is set,
     * and without it the wapp would never be marked dead and the supervisor
     * loop would spin forever. */
    WA_threadEnd(d);

    pthread_exit(NULL);
}

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;

    if (NULL == wapp) {
        return -EINVAL;
    }

    DEBUG_TRACE("Opening: %s\n", path);

    f = fopen(path, "rb");

    if (NULL == f) {
        FATAL(-errno, "can't open wapp: %s", path);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED)
        FATAL(-errno, "can't map file");

    wapp->layers[0] = img;
    wapp->layer_lens[0] = filesize;
    wapp->layer_cnt = 1;

    fclose(f);
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (NULL == wapp) {
        return -EINVAL;
    }

    if (munmap(wapp->layers[0], wapp->layer_lens[0]) < 0)
        FATAL(-errno, "can't unmap file");
    return 0;
}

/* Init task's scheduling priority, captured on the first wapp start (which runs
 * in that task). Wapps run at this base; the supervisor one step above it. */
static int basePriority = -1;

/* Start a worker thread for a wapp. The supervisor runs one scheduling step
 * above the wapps it manages so it can always preempt and terminate a runaway
 * (e.g. a never-yielding wapp). Priorities are set explicitly rather than
 * inherited: a wapp is launched from the supervisor's own (elevated) thread, so
 * inheriting would lift it to the supervisor's priority and defeat preemption.
 * NuttX honours SCHED_RR priorities directly; if host forbids real-time
 * scheduling, it returns EPERM, so all threads fall
 * back to default scheduling, where the host scheduler time-slices regardless. */
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
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    rc = pthread_create(t, &attr, WA_thread, (void *)data);
    pthread_attr_destroy(&attr);
    if (rc == 0)
        return 0;

    /* No privilege for real-time scheduling: use default scheduling. */
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
    state.threads[slot].status = STARTING;
    state.threads[slot].interrupted = 0;

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
    pthread_t worker;

    /* Hold state_mtx across the match, terminate, and signal so the worker
     * cannot be reaped (status flipped, slot reused) out from under us. */
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

    /* Cooperative stop. Set the terminate flag so wasm_runtime_call_wasm returns
     * false at the next instruction boundary, then signal the worker to EINTR
     * any host call it is currently blocked in so that boundary is reached
     * promptly. The thread unwinds through WA_threadEnd; no pthread_cancel. */
    WantedWappTerminate((wapp_data_t *)&state.threads[slot].data);
    worker = state.threads[slot].t;

    pthread_mutex_unlock(&state_mtx);

    /* ESRCH: the worker already exited between the flag set and here — the stop
     * still took effect, so treat it as success. */
    int rc = pthread_kill(worker, WAPP_STOP_SIGNAL);
    if (rc != 0 && rc != ESRCH)
        return -rc;

    return 0;
}

/* NuttX reboot re-execs nothing — a board reset replaces the whole image — so
 * the captured argv is unused. Kept for platform-API symmetry with the host. */
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

void PlatformWappLoop() {
    uint8_t supervisorOk;

    for (;;) {
        sleep(1);

        pthread_mutex_lock(&state_mtx);
        int shutdown = shutdown_requested;
        int reboot = reboot_requested;
        pthread_mutex_unlock(&state_mtx);

        if (shutdown) {
#ifdef __NuttX__
            boardctl(BOARDIOC_POWEROFF, 0);
#endif
            return;
        }
        if (reboot) {
#ifdef __NuttX__
            boardctl(BOARDIOC_RESET, 0);
#endif
            return;
        }

        supervisorOk = 0;
        for (int i = 0; i < MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (state.threads[i].data.wapp == NULL)
                continue;

            if (strncmp((const char *)state.threads[i].data.wapp->name,
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

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
        if (state.threads[i].data.wapp == NULL)
            continue;

        strncpy(wapps[r].name, (const char *)state.threads[i].data.wapp->name,
                WAPP_MAX_NAME_LEN);
        wapps[r].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        wapps[r].status = state.threads[i].status;
        wapps[r].version = state.threads[i].data.wapp->version;
        wapps[r].id = state.threads[i].data.id;
        r++;
    }

    return r;
}
