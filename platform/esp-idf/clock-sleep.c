/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF sleep; resolution and current-time queries are the shared POSIX
 * clock.c. ESP-IDF provides usleep but not clock_nanosleep, so an absolute
 * deadline becomes a relative wait against the same clock. */

#include <errno.h>
#include <stdint.h>
#include <unistd.h>

#include <freertos/FreeRTOS.h>

#include <clock-posix.h>
#include <platform.h>

#define NS_PER_US 1000ULL
#define USLEEP_CHUNK_US 1000000000U /* keep each usleep within useconds_t */

/* One tick of the scheduler. Below it usleep spins on the CPU instead of
 * blocking, which starves the idle task and trips the task watchdog, thus a
 * sleep of any length waits at least this long.
 */

#define TICK_US ((uint64_t)portTICK_PERIOD_MS * 1000ULL)

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
    if (us > 0 && us < TICK_US) {
        us = TICK_US;
    }

    while (us > 0) {
        useconds_t chunk =
            (us > USLEEP_CHUNK_US) ? USLEEP_CHUNK_US : (useconds_t)us;
        usleep(chunk);
        us -= chunk;
    }
    return 0;
}
