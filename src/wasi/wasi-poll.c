/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>
#include <vfs.h>

#include "wasi-internal.h"

/* Recheck cadence while a descriptor subscription waits, matching the drivers'
 * own blocking reads. */
#define POLL_INTERVAL_NS 1000000ULL /* 1 ms */

/* A clock-only wait still wakes this often to honour a stop raised through
 * the wake descriptor. */
#define POLL_WAKE_SLICE_NS 100000000ULL /* 100 ms */

#define POLL_CLOCKS (PLAT_CLOCKID_THREAD_CPUTIME_ID + 1U)

/* Start of the call per clock: relative timeouts count from here. */
typedef struct {
    plat_timestamp_t start[POLL_CLOCKS];
    bool have[POLL_CLOCKS];
} poll_clocks_t;

/* True when the clock subscription fired; otherwise narrows *wait to the time
 * left until it does. */
static bool clockFired(poll_clocks_t *pc, const __wasi_subscription_t *s,
                       __wasi_event_t *ev, uint64_t *wait) {
    uint32_t id = s->u.clock.id;
    plat_timestamp_t now = 0;

    if (id >= POLL_CLOCKS || PlatformClockGetTime(id, &now) < 0) {
        ev->error = __WASI_ERRNO_INVAL;
        return true;
    }
    if (!pc->have[id]) {
        pc->start[id] = now;
        pc->have[id] = true;
    }

    uint64_t timeout = s->u.clock.timeout;
    uint64_t deadline = timeout;
    if (!(s->u.clock.flags & __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME))
        deadline = (timeout > UINT64_MAX - pc->start[id])
                       ? UINT64_MAX
                       : pc->start[id] + timeout;
    if (now >= deadline)
        return true;
    if (deadline - now < *wait)
        *wait = deadline - now;
    return false;
}

static bool fdFired(vfs_ctx_t c, const __wasi_subscription_t *s,
                    __wasi_event_t *ev) {
    uint32_t avail = 0;
    int mask = VfsPoll(c, (int)s->u.fd_readwrite.fd, &avail);

    if (mask < 0) {
        ev->error = WasiErrno(mask);
        return true;
    }
    bool read = s->type == __WASI_EVENTTYPE_FD_READ;
    if (!(mask & (VFS_POLL_HUP | (read ? VFS_POLL_IN : VFS_POLL_OUT))))
        return false;
    ev->fd_readwrite.nbytes = read ? avail : 0;
    if (mask & VFS_POLL_HUP)
        ev->fd_readwrite.flags = __WASI_EVENTRWFLAGS_FD_READWRITE_HANGUP;
    return true;
}

__wasi_errno_t WasiPollOneoff(vfs_ctx_t c, const __wasi_subscription_t *in,
                              __wasi_event_t *out, uint32_t n,
                              uint32_t *nevents) {
    poll_clocks_t pc;
    bool hasFd = false;

    if (n == 0)
        return __WASI_ERRNO_INVAL;
    memset(&pc, 0, sizeof(pc));
    for (uint32_t i = 0; i < n; i++)
        hasFd = hasFd || in[i].type != __WASI_EVENTTYPE_CLOCK;

    for (;;) {
        uint32_t fired = 0;
        uint64_t wait = UINT64_MAX;

        for (uint32_t i = 0; i < n; i++) {
            const __wasi_subscription_t *s = &in[i];
            __wasi_event_t ev;
            bool hit;

            memset(&ev, 0, sizeof(ev));
            ev.userdata = s->userdata;
            ev.type = s->type;
            switch (s->type) {
            case __WASI_EVENTTYPE_CLOCK:
                hit = clockFired(&pc, s, &ev, &wait);
                break;
            case __WASI_EVENTTYPE_FD_READ:
            case __WASI_EVENTTYPE_FD_WRITE:
                hit = fdFired(c, s, &ev);
                break;
            default:
                ev.error = __WASI_ERRNO_INVAL;
                hit = true;
                break;
            }
            if (hit)
                out[fired++] = ev;
        }
        if (fired > 0) {
            *nevents = fired;
            return __WASI_ERRNO_SUCCESS;
        }

        int wakeFd = VfsWakeFd(c);
        uint64_t slice = wait;
        if (hasFd)
            slice = POLL_INTERVAL_NS;
        else if (wakeFd >= 0)
            slice = POLL_WAKE_SLICE_NS;
        if (slice > wait)
            slice = wait;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, slice, 0) == -EINTR)
            return __WASI_ERRNO_INTR;
        if (PlatformWakeRaised(wakeFd))
            return __WASI_ERRNO_INTR;
    }
}
