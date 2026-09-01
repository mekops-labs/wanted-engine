/* SPDX-License-Identifier: Apache-2.0 */

#ifndef BOARD_OTA_H
#define BOARD_OTA_H

/* Boot the slot a committed image was staged into, if one is pending.
 * Returns when nothing is staged, leaving the caller to reset as it would
 * have; does not return when a staged image is booted.
 *
 * The loader only treats a boot as provisional when the reset itself names
 * the updated slot, so an ordinary reset after a commit would run the new
 * image with nothing armed to revert it. */
void BoardOtaBootPending(void);

#endif /* BOARD_OTA_H */
