/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Board bring-up before the engine starts: power rails, shared buses. Supplied
 * by an out-of-tree board directory; the default does nothing. Returns 0 on
 * success, a negative errno otherwise. */
int BoardInit(void);

/* Called about once a second while the engine loop runs. */
void BoardHeartbeat(void);

/* Cuts board power on poweroff. Returns when it cannot; deep sleep follows. */
void BoardPowerOff(void);
