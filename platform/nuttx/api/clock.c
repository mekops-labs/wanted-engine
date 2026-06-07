#include <errno.h>
#include <time.h>

#include <platform.h>
#include <wapp-stop.h>

static inline int convert_clockid(plat_clk_id_t in) {
    switch (in) {
    case PLAT_CLOCKID_REALTIME:
        return CLOCK_REALTIME;
    case PLAT_CLOCKID_MONOTONIC:
        return CLOCK_MONOTONIC;
    /* NuttX also defines CLOCK_PROCESS_CPUTIME_ID / CLOCK_THREAD_CPUTIME_ID,
     * but they stay unsupported here for parity with the Linux platform. */
    default:
        return -1;
    }
}

static inline plat_timestamp_t convert_timespec(const struct timespec *ts) {
    if (ts->tv_sec < 0)
        return 0;
    if ((plat_timestamp_t)ts->tv_sec >= UINT64_MAX / 1000000000)
        return UINT64_MAX;
    return (plat_timestamp_t)ts->tv_sec * 1000000000 + ts->tv_nsec;
}

static inline struct timespec convert_timestamp(plat_timestamp_t t) {
    struct timespec ts;

    ts.tv_sec = t / 1000000000ULL;
    ts.tv_nsec = t % 1000000000LL;

    return ts;
}

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

int PlatformClockNanoSleep(plat_clk_id_t clk_id, plat_timestamp_t timeout,
                           plat_clk_flags_t flags) {
    struct timespec rqtp = convert_timestamp(timeout);

    /* clock_nanosleep returns the error number directly (it does not set
     * errno). Surface any error so a genuine failure propagates. */
    int rc = clock_nanosleep(convert_clockid(clk_id), flags, &rqtp, NULL);
    if (rc != 0)
        return -rc;

    /* The stop signal wakes this sleep early but NuttX reports success, so the
     * timer return alone cannot distinguish an interrupt from elapsed time.
     * Consume the per-worker interrupt flag the stop handler set and report it
     * as EINTR. This unwinds the host call — including the pipe driver's poll
     * loop, which bails on EINTR — back to the interpreter, where the terminate
     * flag is honoured. */
    if (PlatformStopInterruptConsume())
        return -EINTR;

    return 0;
}
