/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>

#if SECURE_SOCKETS

#include <openssl/evp.h>

int PlatformEd25519Verify(const uint8_t pubkey[PLATFORM_ED25519_KEY_LEN],
                          const uint8_t sig[PLATFORM_ED25519_SIG_LEN],
                          const uint8_t *msg, size_t msgLen) {
    if (pubkey == NULL || sig == NULL || (msg == NULL && msgLen > 0))
        return -EINVAL;

    EVP_PKEY *pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL, pubkey,
                                                 PLATFORM_ED25519_KEY_LEN);
    if (pkey == NULL)
        return -EINVAL;

    EVP_MD_CTX *md = EVP_MD_CTX_new();
    if (md == NULL) {
        EVP_PKEY_free(pkey);
        return -ENOMEM;
    }

    int ret = -EIO;
    if (EVP_DigestVerifyInit(md, NULL, NULL, NULL, pkey) == 1) {
        int ok =
            EVP_DigestVerify(md, sig, PLATFORM_ED25519_SIG_LEN, msg, msgLen);
        ret = (ok == 1) ? 0 : -EBADMSG;
    }

    EVP_MD_CTX_free(md);
    EVP_PKEY_free(pkey);
    return ret;
}

#else /* !SECURE_SOCKETS */

int PlatformEd25519Verify(const uint8_t pubkey[PLATFORM_ED25519_KEY_LEN],
                          const uint8_t sig[PLATFORM_ED25519_SIG_LEN],
                          const uint8_t *msg, size_t msgLen) {
    (void)pubkey;
    (void)sig;
    (void)msg;
    (void)msgLen;
    return -ENOSYS;
}

#endif /* SECURE_SOCKETS */
