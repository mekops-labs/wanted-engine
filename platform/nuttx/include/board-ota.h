/* SPDX-License-Identifier: Apache-2.0 */

#ifndef BOARD_OTA_H
#define BOARD_OTA_H

#include <stddef.h>

/* Boot the slot a committed image was staged into, if one is pending.
 * Returns when nothing is staged, leaving the caller to reset as it would
 * have; does not return when a staged image is booted.
 *
 * The loader only treats a boot as provisional when the reset itself names
 * the updated slot, so an ordinary reset after a commit would run the new
 * image with nothing armed to revert it. */
void BoardOtaBootPending(void);

/* Lowercase hex SHA-256 of the running image, taken from the hash the seal
 * writes into it. `bufLen` holds FIRMWARE_DIGEST_HEX_LEN + 1. */
int BoardImageDigestRunning(char *buf, size_t bufLen);

#endif /* BOARD_OTA_H */
