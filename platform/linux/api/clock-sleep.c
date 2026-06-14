/* SPDX-License-Identifier: Apache-2.0 */

#include <time.h>

#include <clock-posix.h>
#include <platform.h>

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    struct timespec rqtp = convert_timestamp(timeout);

    clock_nanosleep(convert_clockid(clk_id), flags, &rqtp, NULL);

    return 0;
}
