/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF sleep. Resolution + current-time queries are the shared POSIX
 * clock.c; only the sleep is platform-specific. ESP-IDF provides usleep but not
 * nanosleep/clock_nanosleep, so an absolute deadline is converted to a relative
 * wait against the same clock. Interruptible stop (unwinding a blocked wapp on
 * terminate) is not wired here yet. */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <clock-posix.h>
#include <platform.h>

#define NS_PER_US 1000ULL
#define USLEEP_CHUNK_US 1000000000U /* keep each usleep within useconds_t */

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    if (convert_clockid(clk_id) < 0)
        return -EINVAL;

    plat_timestamp_t duration = timeout;
    if (flags & PLAT_CLOCK_FLAGS_ABSTIME) {
        plat_timestamp_t now = 0;
        int rc = PlatformClockGetTime(clk_id, &now);
        if (rc < 0)
            return rc;
        duration = (timeout > now) ? (timeout - now) : 0;
    }

    uint64_t us = (duration + (NS_PER_US - 1)) / NS_PER_US;
    while (us > 0) {
        useconds_t chunk =
            (us > USLEEP_CHUNK_US) ? USLEEP_CHUNK_US : (useconds_t)us;
        usleep(chunk);
        us -= chunk;
    }
    return 0;
}
