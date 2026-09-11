/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX hardware watchdog over the /dev/watchdog0 character device. A build
 * without CONFIG_WATCHDOG (the sim, among others) declares no ioctls for it,
 * so the whole body compiles out and every entry point becomes a no-op. */

#include <stdbool.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#ifdef CONFIG_WATCHDOG
#include <fcntl.h>
#include <nuttx/timers/watchdog.h>
#include <pthread.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#endif

#include <board-wdt.h>
#include <debug_trace.h>

#ifdef CONFIG_WATCHDOG

#define WDT_DEVPATH "/dev/watchdog0"

static int wdtFd = -1;

bool BoardWdtArm(unsigned timeoutMs) {
    if (wdtFd >= 0)
        return true;

    int fd = open(WDT_DEVPATH, O_RDONLY);
    if (fd < 0) {
        DEBUG_TRACE("no watchdog at %s", WDT_DEVPATH);
        return false;
    }

    /* Order matters: the timeout is what START then latches. */
    if (ioctl(fd, WDIOC_SETTIMEOUT, (unsigned long)timeoutMs) < 0 ||
        ioctl(fd, WDIOC_START, 0) < 0) {
        DEBUG_TRACE("watchdog rejected a %u ms timeout", timeoutMs);
        close(fd);
        return false;
    }

    wdtFd = fd;
    DEBUG_TRACE("watchdog armed at %u ms", timeoutMs);
    return true;
}

void BoardWdtKick(void) {
    if (wdtFd >= 0)
        ioctl(wdtFd, WDIOC_KEEPALIVE, 0);
}

/* Kicker state. `kickerRunning` doubles as the run flag, so disarming stops
 * the thread at its next wake. */
static pthread_t kickerThread;
static unsigned kickerIntervalMs;
static bool kickerRunning;
static volatile unsigned long heartbeatMs;

/* How stale a heartbeat may be before the kicker lets the board reset. It has
 * to clear the longest legitimate starvation of the engine's main loop, which
 * a wapp at a higher priority can hold off for seconds per reconcile. */
#define WDT_LIVENESS_MS 30000UL

static unsigned long nowMs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (unsigned long)ts.tv_sec * 1000UL +
           (unsigned long)(ts.tv_nsec / 1000000L);
}

void BoardWdtHeartbeat(void) { heartbeatMs = nowMs(); }

static void *kickerMain(void *arg) {
    (void)arg;
    while (kickerRunning) {
        usleep(kickerIntervalMs * 1000U);
        if (nowMs() - heartbeatMs < WDT_LIVENESS_MS)
            BoardWdtKick();
    }
    return NULL;
}

bool BoardWdtStartKicker(unsigned intervalMs, int priority) {
    pthread_attr_t attr;
    struct sched_param sp;
    int hi;

    if (wdtFd < 0 || kickerRunning || intervalMs == 0)
        return false;

    hi = sched_get_priority_max(SCHED_RR);
    if (hi > 0 && priority > hi)
        priority = hi;
    sp.sched_priority = priority;

    kickerIntervalMs = intervalMs;
    heartbeatMs = nowMs();
    kickerRunning = true;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2048);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_RR);
    pthread_attr_setschedparam(&attr, &sp);
    if (pthread_create(&kickerThread, &attr, kickerMain, NULL) != 0) {
        kickerRunning = false;
        DEBUG_TRACE("watchdog kicker did not start at priority %d", priority);
        return false;
    }
    pthread_detach(kickerThread);
    DEBUG_TRACE("watchdog kicked every %u ms at priority %d", intervalMs,
                priority);
    return true;
}

void BoardWdtDisarm(void) {
    kickerRunning = false;
    if (wdtFd < 0)
        return;
    ioctl(wdtFd, WDIOC_STOP, 0);
    close(wdtFd);
    wdtFd = -1;
}

#else /* CONFIG_WATCHDOG */

bool BoardWdtArm(unsigned timeoutMs) {
    (void)timeoutMs;
    return false;
}

void BoardWdtKick(void) {}

void BoardWdtHeartbeat(void) {}

bool BoardWdtStartKicker(unsigned intervalMs, int priority) {
    (void)intervalMs;
    (void)priority;
    return false;
}

void BoardWdtDisarm(void) {}

#endif /* CONFIG_WATCHDOG */
