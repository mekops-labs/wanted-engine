/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <platform.h>

/* HMAC-SHA256 (RFC 2104), built on the portable PlatformSha256* primitives.
 * Returns 0, or -ENOMEM if a digest context cannot be allocated. */
int WantedHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg,
                     size_t msgLen, uint8_t out[PLATFORM_SHA256_DIGEST_LEN]);
