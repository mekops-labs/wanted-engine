/* SPDX-License-Identifier: Apache-2.0 */

#include <time.h>

#include <clock-posix.h>
#include <platform.h>

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    struct timespec rqtp = convert_timestamp(timeout);

    /* clock_nanosleep returns the error number directly (it does not set
     * errno). Surface it so a stop signal's EINTR unwinds the host call —
     * including the pipe driver's poll loop — back to the interpreter, where
     * the terminate flag is honoured. */
    int rc = clock_nanosleep(convert_clockid(clk_id), flags, &rqtp, NULL);
    if (rc != 0)
        return -rc;

    return 0;
}
