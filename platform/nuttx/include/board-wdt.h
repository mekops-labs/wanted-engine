/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Board-level hardware watchdog: resets the whole board when it is not kicked
 * within the timeout. Complements the per-slot revert path — this catches an
 * image that runs but wedges, where nothing else is left to notice.
 *
 * Every call is safe on a board with no watchdog device: arming fails once,
 * quietly, and the rest become no-ops. */

#include <stdbool.h>

/* Arm the watchdog at `timeoutMs`. False if the platform has no watchdog or
 * rejects the timeout, in which case kicking and disarming do nothing. */
bool BoardWdtArm(unsigned timeoutMs);

/* Defer the reset by another full timeout. */
void BoardWdtKick(void);

/* Disarm, so an orderly reboot or poweroff is not raced by a reset. */
void BoardWdtDisarm(void);
