/* SPDX-License-Identifier: Apache-2.0 */

#include <board.h>

/* Defaults for a build with no out-of-tree board directory. */
int BoardInit(void) { return 0; }

void BoardHeartbeat(void) {}

void BoardPowerOff(void) {}
