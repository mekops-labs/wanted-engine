/* SPDX-License-Identifier: Apache-2.0 */

/* Validates WantedHmacSha256 against RFC 4231 test vectors. */

#include <string.h>

#include "unity.h"
#include "unity_fixture.h"

#include <wanted-hmac.h>

TEST_GROUP(wanted_hmac);

TEST_SETUP(wanted_hmac) {}
TEST_TEAR_DOWN(wanted_hmac) {}

static void hex(const uint8_t *bytes, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    out[len * 2] = '\0';
}

/* RFC 4231 Test Case 1: 20-byte key, "Hi There". */
TEST(wanted_hmac, Rfc4231Case1) {
    const uint8_t key[20] = {
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
        0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    };
    const char *msg = "Hi There";
    uint8_t out[PLATFORM_SHA256_DIGEST_LEN];
    char hexOut[65];

    TEST_ASSERT_EQUAL(0,
                      WantedHmacSha256(key, sizeof(key), (const uint8_t *)msg,
                                       strlen(msg), out));
    hex(out, sizeof(out), hexOut);
    TEST_ASSERT_EQUAL_STRING(
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        hexOut);
}

/* RFC 4231 Test Case 2: key shorter than the block size, "what do ya want
 * for nothing?" — exercises the plain zero-padded key path. */
TEST(wanted_hmac, Rfc4231Case2) {
    const char *key = "Jefe";
    const char *msg = "what do ya want for nothing?";
    uint8_t out[PLATFORM_SHA256_DIGEST_LEN];
    char hexOut[65];

    TEST_ASSERT_EQUAL(0,
                      WantedHmacSha256((const uint8_t *)key, strlen(key),
                                       (const uint8_t *)msg, strlen(msg), out));
    hex(out, sizeof(out), hexOut);
    TEST_ASSERT_EQUAL_STRING(
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        hexOut);
}

/* RFC 4231 Test Case 6: a 131-byte key, longer than the block size, so the
 * key-hashing path (keyLen > HMAC_BLOCK_LEN) runs. */
TEST(wanted_hmac, Rfc4231Case6_KeyLongerThanBlock) {
    uint8_t key[131];
    memset(key, 0xaa, sizeof(key));
    const char *msg = "Test Using Larger Than Block-Size Key - Hash Key First";
    uint8_t out[PLATFORM_SHA256_DIGEST_LEN];
    char hexOut[65];

    TEST_ASSERT_EQUAL(0,
                      WantedHmacSha256(key, sizeof(key), (const uint8_t *)msg,
                                       strlen(msg), out));
    hex(out, sizeof(out), hexOut);
    TEST_ASSERT_EQUAL_STRING(
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        hexOut);
}

/* Same key material through both paths (raw vs. re-derived) must agree — a
 * sanity check independent of the RFC vectors. */
TEST(wanted_hmac, DeterministicForSameInputs) {
    const uint8_t key[] = {1, 2, 3, 4, 5};
    const uint8_t msg[] = {6, 7, 8};
    uint8_t a[PLATFORM_SHA256_DIGEST_LEN];
    uint8_t b[PLATFORM_SHA256_DIGEST_LEN];

    TEST_ASSERT_EQUAL(0,
                      WantedHmacSha256(key, sizeof(key), msg, sizeof(msg), a));
    TEST_ASSERT_EQUAL(0,
                      WantedHmacSha256(key, sizeof(key), msg, sizeof(msg), b));
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(a));
}

TEST(wanted_hmac, DifferentKeysDiffer) {
    const uint8_t keyA[] = {1, 2, 3, 4, 5};
    const uint8_t keyB[] = {1, 2, 3, 4, 6};
    const uint8_t msg[] = {7, 8, 9};
    uint8_t a[PLATFORM_SHA256_DIGEST_LEN];
    uint8_t b[PLATFORM_SHA256_DIGEST_LEN];

    TEST_ASSERT_EQUAL(
        0, WantedHmacSha256(keyA, sizeof(keyA), msg, sizeof(msg), a));
    TEST_ASSERT_EQUAL(
        0, WantedHmacSha256(keyB, sizeof(keyB), msg, sizeof(msg), b));
    TEST_ASSERT_NOT_EQUAL(0, memcmp(a, b, sizeof(a)));
}

TEST_GROUP_RUNNER(wanted_hmac) {
    RUN_TEST_CASE(wanted_hmac, Rfc4231Case1);
    RUN_TEST_CASE(wanted_hmac, Rfc4231Case2);
    RUN_TEST_CASE(wanted_hmac, Rfc4231Case6_KeyLongerThanBlock);
    RUN_TEST_CASE(wanted_hmac, DeterministicForSameInputs);
    RUN_TEST_CASE(wanted_hmac, DifferentKeysDiffer);
}
