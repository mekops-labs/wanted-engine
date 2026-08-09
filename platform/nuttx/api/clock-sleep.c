/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <time.h>

#include <clock-posix.h>
#include <platform.h>
#include <wapp-stop.h>

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    struct timespec rqtp = convert_timestamp(timeout);

    /* clock_nanosleep returns the error number directly (it does not set
     * errno). Surface any error so a genuine failure propagates. */
    int rc = clock_nanosleep(convert_clockid(clk_id), flags, &rqtp, NULL);
    if (rc != 0)
        return -rc;

    /* The stop signal wakes this sleep early but NuttX reports success, so the
     * timer return cannot tell an interrupt from an elapsed wait. Consume the
     * per-worker flag the stop handler set and report EINTR. */
    if (PlatformStopInterruptConsume())
        return -EINTR;

    return 0;
}
