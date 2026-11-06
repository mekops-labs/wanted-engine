/* SPDX-License-Identifier: Apache-2.0 */

/* Ready events on a pthread condition variable. The raise flag is a level, so
 * a raise before the wait is not lost. A stop sets a second flag that no wait
 * clears. */

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include <platform.h>
#include <wanted_malloc.h>

struct plat_ready_t {
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    clockid_t clock;
    bool raised;
    bool stopped;
};

plat_ready_t *PlatformReadyNew(void) {
    plat_ready_t *r = WantedMalloc(sizeof(*r));
    pthread_condattr_t attr;
    bool attr_failed = false;

    if (!r)
        return NULL;
    r->raised = false;
    r->stopped = false;
    r->clock = CLOCK_MONOTONIC;
    if (pthread_mutex_init(&r->mtx, NULL) != 0) {
        WantedFree(r);
        return NULL;
    }
    /* A platform that implements no condition attributes waits on the wall
     * clock with the default condition. */
    if (pthread_condattr_init(&attr) == 0) {
        if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0)
            r->clock = CLOCK_REALTIME;
        if (pthread_cond_init(&r->cond, &attr) != 0)
            attr_failed = true;
        pthread_condattr_destroy(&attr);
    } else {
        r->clock = CLOCK_REALTIME;
        attr_failed = pthread_cond_init(&r->cond, NULL) != 0;
    }
    if (attr_failed) {
        pthread_mutex_destroy(&r->mtx);
        WantedFree(r);
        return NULL;
    }
    return r;
}

void PlatformReadyFree(plat_ready_t *r) {
    if (!r)
        return;
    pthread_cond_destroy(&r->cond);
    pthread_mutex_destroy(&r->mtx);
    WantedFree(r);
}

void PlatformReadyRaise(plat_ready_t *r) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mtx);
    r->raised = true;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->mtx);
}

void PlatformReadyStop(plat_ready_t *r) {
    if (!r)
        return;
    pthread_mutex_lock(&r->mtx);
    r->stopped = true;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->mtx);
}

static void deadlineAfter(clockid_t clock, int64_t ns, struct timespec *ts) {
    clock_gettime(clock, ts);
    ts->tv_sec += ns / 1000000000LL;
    ts->tv_nsec += (long)(ns % 1000000000LL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

int PlatformReadyWait(plat_ready_t *r, int64_t timeoutNs) {
    struct timespec until = {0, 0};
    int ret = 0;

    if (!r)
        return -EINVAL;
    if (timeoutNs >= 0)
        deadlineAfter(r->clock, timeoutNs, &until);
    pthread_mutex_lock(&r->mtx);
    while (!r->raised && !r->stopped) {
        int rc = timeoutNs >= 0
                     ? pthread_cond_timedwait(&r->cond, &r->mtx, &until)
                     : pthread_cond_wait(&r->cond, &r->mtx);
        if (rc == ETIMEDOUT) {
            ret = -ETIMEDOUT;
            break;
        }
    }
    if (ret == 0) {
        if (r->stopped)
            ret = -EINTR;
        else
            r->raised = false;
    }
    pthread_mutex_unlock(&r->mtx);
    return ret;
}
