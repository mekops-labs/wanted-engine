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
#include <sys/ioctl.h>
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

void BoardWdtDisarm(void) {
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

void BoardWdtDisarm(void) {}

#endif /* CONFIG_WATCHDOG */
