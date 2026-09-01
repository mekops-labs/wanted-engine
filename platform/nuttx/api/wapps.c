/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX platform wapp lifecycle: detached pthreads, one slot per wapp, and a
 * cooperative stop built from the terminate flag plus a signal that wakes a
 * blocked host call. See the platform guide. */

#include <errno.h>
#include <limits.h>
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

#include <board-ota.h>
#include <board-wdt.h>
#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>
#include <wanted_malloc.h>
#include <wapp-stop.h>

#include <debug_trace.h>
#include <wanted_log.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

/* Signal used to interrupt a worker blocked in a host syscall. SIGUSR2, because
 * WAMR reserves SIGUSR1 for its own wakeup and keeps it masked on every wasm
 * thread, so a SIGUSR1 sent here would never reach the worker. */
#define WAPP_STOP_SIGNAL SIGUSR2

/* Board watchdog timeout. Kicked once per run-loop iteration, which sleeps a
 * second, so this is the margin for a loop that is late rather than wedged.
 * The rp23xx counter is 24 bits of microseconds, capping it near 16.7 s. */
#define BOARD_WDT_TIMEOUT_MS 8000

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
    thread_data_t threads[CONFIG_WANTED_MAX_WAPPS];
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

/* Stop signal handler. Delivery wakes the worker's blocking host call early and
 * records the interrupt on its slot, so PlatformClockNanoSleep can report
 * EINTR. It deliberately neither pthread_exits nor pthread_cancels. */
static void stopSigHandler(int sig) {
    (void)sig;
    pthread_t self = pthread_self();
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (pthread_equal(state.threads[i].t, self)) {
            state.threads[i].interrupted = 1;
            break;
        }
    }
}

/* Read and clear the calling worker's pending stop interrupt (see wapp-stop.h).
 */
bool PlatformStopInterruptConsume(void) {
    pthread_t self = pthread_self();
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
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

    /* Install the stop handler with SA_RESTART cleared and unblock the signal
     * on this worker, so pthread_kill can interrupt a blocked host call. The
     * disposition is process-wide and idempotent; the mask is per-thread. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags =
        0; /* no SA_RESTART → the blocked call returns, not restarts */
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

    /* The stop path is cooperative, so the worker always reaches here on a
     * normal return and the teardown is called directly. A pthread_cleanup
     * handler is a no-op unless CONFIG_PTHREAD_CLEANUP is set. */
    WA_threadEnd(d);

    pthread_exit(NULL);
}

/* Init task's scheduling priority, captured on the first wapp start (which runs
 * in that task). Wapps run at this base; the supervisor one step above it. */
static int basePriority = -1;

/* Worker thread's native C stack, set explicitly from
 * CONFIG_WANTED_WASM_WORKER_STACK_SIZE and floored at PTHREAD_STACK_MIN. The
 * NuttX per-thread default overflows the moment real wasm runs. */
static size_t worker_stacksize(void) {
    size_t ss = CONFIG_WANTED_WASM_WORKER_STACK_SIZE;
#ifdef PTHREAD_STACK_MIN
    if (ss < (size_t)PTHREAD_STACK_MIN)
        ss = (size_t)PTHREAD_STACK_MIN;
#endif
    return ss;
}

size_t PlatformWorkerStackSize(void) { return worker_stacksize(); }

/* Start a worker thread for a wapp. The supervisor runs one scheduling step
 * above the wapps it manages, and priorities are set explicitly rather than
 * inherited. See the platform guide for why, and for the EPERM fallback. */
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
    if (state.n >= CONFIG_WANTED_MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    /* A terminal slot still holds its wapp's record until `delete`
     * releases it, so only an empty slot is free to take. A wapp
     * restarting in place keeps the slot it already holds. */
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
    state.threads[slot].interrupted = 0;

    int rc = startWorker((pthread_t *)&state.threads[slot].t,
                         (wapp_data_t *)&state.threads[slot].data,
                         wapp == WantedGetCurrentSupervisor());
    if (rc != 0) {
        /* pthread_create failed both attempts, so nothing will ever move the
         * slot past STARTING. Free it for reuse; the caller owns unloading
         * `wapp` on a negative return, so this must not touch it further. */
        state.threads[slot].status = NOT_STARTED;
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
    pthread_t worker;

    /* Hold state_mtx across the match, terminate, and signal so the worker
     * cannot be reaped (status flipped, slot reused) out from under us. */
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
     * returns false at the next instruction boundary, then signal the worker so
     * a blocked host call EINTRs and that boundary is reached promptly. */
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

    /* Only a terminal slot can be released: a running or starting wapp still
     * has a worker dereferencing it, so freeing the image would be a
     * use-after-free. A terminal status guarantees the thread is gone. */
    if (state.threads[slot].status != EXITED &&
        state.threads[slot].status != FAILURE) {
        pthread_mutex_unlock(&state_mtx);
        return -EBUSY;
    }

    /* Free the mapped image and struct; the supervisor image is a singleton
     * reused across respawns. state.n was decremented when the worker reached
     * its terminal status, so it must not be decremented again. */
    wapp_t *w = state.threads[slot].data.wapp;
    if (w != NULL && w != WantedGetCurrentSupervisor()) {
        PlatformWappUnload(w);
        WantedFree(w);
    }
    state.threads[slot].data.wapp = NULL;
    state.threads[slot].data.id = 0;
    state.threads[slot].data.exit_code = WAPP_EXIT_CODE_NONE;
    state.threads[slot].status = NOT_STARTED;
    state.threads[slot].interrupted = 0;

    pthread_mutex_unlock(&state_mtx);
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

void PlatformWappLoop(void) {
    bool supervisorOk;
    bool otaConfirmed = false;

    /* Armed here rather than before the engine starts: nothing kicks it until
     * this loop runs, and a boot that never reaches the loop is what the OTA
     * revert path exists to catch. */
    BoardWdtArm(BOARD_WDT_TIMEOUT_MS);

    for (;;) {
        sleep(1);
        BoardWdtKick();

        pthread_mutex_lock(&state_mtx);
        int shutdown = shutdown_requested;
        int reboot = reboot_requested;
        pthread_mutex_unlock(&state_mtx);

        if (shutdown) {
            BoardWdtDisarm();
#ifdef __NuttX__
            boardctl(BOARDIOC_POWEROFF, 0);
#endif
            return;
        }
        if (reboot) {
            BoardWdtDisarm();
            /* Boots a committed image as provisional; returns if none is
             * staged, and the ordinary reset below then runs. */
            BoardOtaBootPending();
#ifdef __NuttX__
            boardctl(BOARDIOC_RESET, 0);
#endif
            return;
        }

        supervisorOk = false;
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
                /* WantedWappRun's negative return — the launch error (e.g.
                 * -EINVAL for a malformed mount, -EROFS for a missing backing
                 * dir). */
                supervisorErr = state.threads[i].data.lastStatus;
            }
        }

        switch (WantedSupervisorObserve(supervisorOk, supervisorFailed,
                                        supervisorExited)) {
        case SUPERVISOR_HEALTHY:
            /* The supervisor reached RUNNING at least once this boot, so the
             * image is good. Idempotent, and a no-op on a confirmed slot. */
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
            BoardWdtDisarm();
            return;
        }
        PlatformWappStart(WantedGetCurrentSupervisor());
    }
}

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < CONFIG_WANTED_MAX_WAPPS && r < appsLen; i++) {
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
        /* Only a running wapp has a live WAMR instance to sample; sampling a
         * terminated slot would chase a torn-down instance. */
        WantedWappMemStats(
            wapps[r].status == RUNNING ? state.threads[i].data.wamr : NULL,
            &wapps[r]);
        r++;
    }

    return r;
}
