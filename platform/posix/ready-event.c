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

#define NS_PER_S 1000000000LL
/* Longest single timed wait on the wall clock, so a clock set back costs at
 * most this much extra sleep. */
#define REALTIME_STEP_NS (10 * NS_PER_S)

/* The largest time_t, for an integer time_t of 32 or 64 bits. */
#define TIME_T_MAX                                                             \
    ((time_t)(sizeof(time_t) == 4 ? (int64_t)INT32_MAX : INT64_MAX))

static bool nowNs(clockid_t clock, int64_t *ns) {
    struct timespec ts;

    if (clock_gettime(clock, &ts) != 0)
        return false;
    *ns = (int64_t)ts.tv_sec * NS_PER_S + ts.tv_nsec;
    return true;
}

/* `ns` from now on `clock`, saturated at the largest representable time. */
static bool deadlineAfter(clockid_t clock, int64_t ns, struct timespec *ts) {
    if (clock_gettime(clock, ts) != 0)
        return false;
    int64_t secs = ns / NS_PER_S;
    ts->tv_nsec += (long)(ns % NS_PER_S);
    if (ts->tv_nsec >= NS_PER_S) {
        secs++;
        ts->tv_nsec -= NS_PER_S;
    }
    if (secs > (int64_t)(TIME_T_MAX - ts->tv_sec)) {
        ts->tv_sec = TIME_T_MAX;
        ts->tv_nsec = NS_PER_S - 1;
    } else {
        ts->tv_sec += (time_t)secs;
    }
    return true;
}

int PlatformReadyWait(plat_ready_t *r, int64_t timeoutNs) {
    struct timespec until = {0, 0};
    int64_t end = 0;
    int ret = 0;

    if (!r)
        return -EINVAL;
    /* The time left is kept on the monotonic clock; each timed wait on the
     * wall clock is cut to REALTIME_STEP_NS. */
    if (timeoutNs >= 0) {
        int64_t now;
        if (!nowNs(CLOCK_MONOTONIC, &now))
            return -EINVAL;
        end = now > INT64_MAX - timeoutNs ? INT64_MAX : now + timeoutNs;
    }
    pthread_mutex_lock(&r->mtx);
    while (!r->raised && !r->stopped) {
        if (timeoutNs < 0) {
            pthread_cond_wait(&r->cond, &r->mtx);
            continue;
        }
        int64_t now;
        if (!nowNs(CLOCK_MONOTONIC, &now)) {
            ret = -EINVAL;
            break;
        }
        int64_t left = end - now;
        if (left <= 0) {
            ret = -ETIMEDOUT;
            break;
        }
        if (r->clock != CLOCK_MONOTONIC && left > REALTIME_STEP_NS)
            left = REALTIME_STEP_NS;
        if (!deadlineAfter(r->clock, left, &until)) {
            ret = -EINVAL;
            break;
        }
        pthread_cond_timedwait(&r->cond, &r->mtx, &until);
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
