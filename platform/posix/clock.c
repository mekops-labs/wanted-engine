/* SPDX-License-Identifier: Apache-2.0 */

/* Shared POSIX clock queries (resolution + current time), used by every POSIX
 * platform target. PlatformClockNanoSleep is implemented per-platform. */

#include <errno.h>
#include <time.h>

#include <clock-posix.h>
#include <platform.h>

int PlatformClockGetRes(plat_clk_id_t clk_id, plat_timestamp_t *resolution) {
    if (resolution == NULL)
        return -EINVAL;

    int clk = convert_clockid(clk_id);
    if (clk < 0)
        return -EINVAL;

    struct timespec tp;
    if (clock_getres(clk, &tp) != 0) {
        *resolution = 1000000ULL;
    } else {
        *resolution = convert_timespec(&tp);
    }

    return 0;
}

int PlatformClockGetTime(plat_clk_id_t clk_id, plat_timestamp_t *time) {
    if (time == NULL)
        return -EINVAL;

    int clk = convert_clockid(clk_id);
    if (clk < 0)
        return -EINVAL;

    struct timespec tp;
    if (clock_gettime(clk, &tp) != 0) {
        return -errno;
    }

    *time = convert_timespec(&tp);

    return 0;
}
