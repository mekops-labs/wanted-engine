/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX's vendored mbedTLS has no Ed25519 support (confirmed against the
 * ESP-IDF port's own finding for the same gap - see the mekops-kb
 * plans/wanted-sheriff-deputy-uart-transport.md), so this backs
 * PlatformEd25519Verify with vendor/ed25519 (orlp/ed25519, verify-only
 * subset) instead - portable C, no hardware crypto peripheral needed. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <ed25519.h>

#include <platform.h>

int PlatformEd25519Verify(const uint8_t pubkey[PLATFORM_ED25519_KEY_LEN],
                          const uint8_t sig[PLATFORM_ED25519_SIG_LEN],
                          const uint8_t *msg, size_t msgLen) {
    if (pubkey == NULL || sig == NULL || (msg == NULL && msgLen > 0))
        return -EINVAL;

    return ed25519_verify(sig, msg, msgLen, pubkey) ? 0 : -EBADMSG;
}
