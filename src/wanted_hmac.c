/* SPDX-License-Identifier: Apache-2.0 */

/* HMAC-SHA256 (RFC 2104) over the portable PlatformSha256* primitives. */

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <wanted-hmac.h>

#define HMAC_BLOCK_LEN 64

int WantedHmacSha256(const uint8_t *key, size_t keyLen, const uint8_t *msg,
                     size_t msgLen, uint8_t out[PLATFORM_SHA256_DIGEST_LEN]) {
    uint8_t keyBlock[HMAC_BLOCK_LEN];
    memset(keyBlock, 0, sizeof(keyBlock));

    if (keyLen > HMAC_BLOCK_LEN) {
        void *ctx = PlatformSha256New();
        if (ctx == NULL)
            return -ENOMEM;
        PlatformSha256Update(ctx, key, keyLen);
        PlatformSha256Final(ctx, keyBlock);
        PlatformSha256Free(ctx);
    } else if (keyLen > 0) {
        memcpy(keyBlock, key, keyLen);
    }

    uint8_t ipad[HMAC_BLOCK_LEN];
    uint8_t opad[HMAC_BLOCK_LEN];
    for (size_t i = 0; i < HMAC_BLOCK_LEN; i++) {
        ipad[i] = (uint8_t)(keyBlock[i] ^ 0x36);
        opad[i] = (uint8_t)(keyBlock[i] ^ 0x5c);
    }

    uint8_t inner[PLATFORM_SHA256_DIGEST_LEN];
    void *ictx = PlatformSha256New();
    if (ictx == NULL)
        return -ENOMEM;
    PlatformSha256Update(ictx, ipad, sizeof(ipad));
    PlatformSha256Update(ictx, msg, msgLen);
    PlatformSha256Final(ictx, inner);
    PlatformSha256Free(ictx);

    void *octx = PlatformSha256New();
    if (octx == NULL)
        return -ENOMEM;
    PlatformSha256Update(octx, opad, sizeof(opad));
    PlatformSha256Update(octx, inner, sizeof(inner));
    PlatformSha256Final(octx, out);
    PlatformSha256Free(octx);

    return 0;
}
