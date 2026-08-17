/* SPDX-License-Identifier: Apache-2.0 */

/* Wake descriptors for a platform whose stop interrupts the worker by signal.
 * The signal ends a blocking call on its own, so there is nothing to watch. */

#include <stdbool.h>

#include <platform.h>

int PlatformWakeCreate(void) { return -1; }

void PlatformWakeRaise(int fd) { (void)fd; }

bool PlatformWakeRaised(int fd) {
    (void)fd;
    return false;
}

void PlatformWakeClose(int fd) { (void)fd; }
