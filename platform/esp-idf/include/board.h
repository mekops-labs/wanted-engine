/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* Board bring-up before the engine starts: power rails, shared buses. Supplied
 * by an out-of-tree board directory; the default does nothing. Returns 0 on
 * success, a negative errno otherwise. */
int BoardInit(void);
