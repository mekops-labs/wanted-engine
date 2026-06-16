/* SPDX-License-Identifier: Apache-2.0 */

/* Shared POSIX clock conversions between the platform clock ABI
 * (plat_clk_id_t / plat_timestamp_t) and <time.h> types. Used by the shared
 * platform/posix/clock.c (PlatformClockGetRes / PlatformClockGetTime) and by
 * each platform's PlatformClockNanoSleep, which stays per-platform — NuttX
 * consumes a per-worker stop interrupt to report EINTR. */

#ifndef PLATFORM_CLOCK_POSIX_H
#define PLATFORM_CLOCK_POSIX_H

#include <stdint.h>
#include <time.h>

#include <platform.h>

static inline int convert_clockid(plat_clk_id_t in) {
    switch (in) {
    case PLAT_CLOCKID_REALTIME:
        return CLOCK_REALTIME;
    case PLAT_CLOCKID_MONOTONIC:
        return CLOCK_MONOTONIC;
    default:
        return -1;
    }
}

static inline plat_timestamp_t convert_timespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((plat_timestamp_t)ts->tv_sec >= UINT64_MAX / 1000000000)
        return UINT64_MAX;
    return ((plat_timestamp_t)ts->tv_sec * 1000000000) + ts->tv_nsec;
}

static inline struct timespec convert_timestamp(plat_timestamp_t t) {
    struct timespec ts;

    ts.tv_sec = t / 1000000000ULL;
    ts.tv_nsec = t % 1000000000LL;

    return ts;
}

#endif /* PLATFORM_CLOCK_POSIX_H */
