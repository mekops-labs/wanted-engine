/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Board-level hardware watchdog: resets the whole board when it is not kicked
 * within the timeout. Safe on a board with no watchdog device: arming fails
 * once, quietly, and the rest become no-ops. See docs/platform-guide.md. */

#include <stdbool.h>

/* Arm the watchdog at `timeoutMs`. False if the platform has no watchdog or
 * rejects the timeout, in which case kicking and disarming do nothing. */
bool BoardWdtArm(unsigned timeoutMs);

/* Defer the reset by another full timeout. */
void BoardWdtKick(void);

/* Kick every `intervalMs` from a thread at `priority`, which must outrank
 * every wapp — see docs/platform-guide.md. False where the thread could not
 * be started, leaving the caller to kick as it did before. */
bool BoardWdtStartKicker(unsigned intervalMs, int priority);

/* Report the engine's main loop alive. The kicker stops kicking when these
 * stop arriving, so a wedged engine still resets the board — the kicker
 * proves the engine is running, it does not replace it. */
void BoardWdtHeartbeat(void);

/* Disarm, so an orderly reboot or poweroff is not raced by a reset. */
void BoardWdtDisarm(void);
