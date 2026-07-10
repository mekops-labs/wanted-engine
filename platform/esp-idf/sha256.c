/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF PlatformSha256* backend: mbedtls_sha256_context transparently uses
 * the ESP32-S3 SHA hardware peripheral whenever MBEDTLS_HARDWARE_SHA is
 * enabled (ESP-IDF's default for this SoC) — no separate API, the ordinary
 * mbedtls_sha256_* calls below are the accelerated path. This is a genuine
 * hardware offload, unlike ECDSA/ECC on this chip (no ECC peripheral exists
 * on ESP32-S3; that only ships on the RISC-V parts). */

#include <stdint.h>

#include "mbedtls/sha256.h"

#include <platform.h>
#include <wanted_malloc.h>

void *PlatformSha256New(void) {
    mbedtls_sha256_context *ctx =
        (mbedtls_sha256_context *)WantedMalloc(sizeof(*ctx));
    if (ctx == NULL)
        return NULL;

    mbedtls_sha256_init(ctx);
    mbedtls_sha256_starts(ctx, 0 /* SHA-256, not SHA-224 */);
    return ctx;
}

void PlatformSha256Update(void *ctxIn, const uint8_t *data, size_t len) {
    mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctxIn;
    mbedtls_sha256_update(ctx, data, len);
}

void PlatformSha256Final(void *ctxIn, uint8_t out[PLATFORM_SHA256_DIGEST_LEN]) {
    mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctxIn;
    mbedtls_sha256_finish(ctx, out);
}

void PlatformSha256Free(void *ctxIn) {
    mbedtls_sha256_context *ctx = (mbedtls_sha256_context *)ctxIn;
    if (ctx == NULL)
        return;
    mbedtls_sha256_free(ctx);
    WantedFree(ctx);
}
