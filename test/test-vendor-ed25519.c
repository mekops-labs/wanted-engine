/* SPDX-License-Identifier: Apache-2.0 */

/* Validates the vendored ed25519_verify() (vendor/ed25519) directly against
 * RFC 8032 §7.1 TEST 1 - independent of PlatformEd25519Verify/platform
 * wiring, so this catches a broken vendor build even before it reaches
 * platform/nuttx/api/crypto.c. */

#include <string.h>

#include <ed25519.h>

#include "unity.h"
#include "unity_fixture.h"

TEST_GROUP(vendor_ed25519);

TEST_SETUP(vendor_ed25519) {}
TEST_TEAR_DOWN(vendor_ed25519) {}

static const unsigned char pubkey[32] = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe,
    0xd3, 0xc9, 0x64, 0x07, 0x3a, 0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6,
    0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

static const unsigned char sig[64] = {
    0xe5, 0x56, 0x43, 0x00, 0xc3, 0x60, 0xac, 0x72, 0x90, 0x86, 0xe2,
    0xcc, 0x80, 0x6e, 0x82, 0x8a, 0x84, 0x87, 0x7f, 0x1e, 0xb8, 0xe5,
    0xd9, 0x74, 0xd8, 0x73, 0xe0, 0x65, 0x22, 0x49, 0x01, 0x55, 0x5f,
    0xb8, 0x82, 0x15, 0x90, 0xa3, 0x3b, 0xac, 0xc6, 0x1e, 0x39, 0x70,
    0x1c, 0xf9, 0xb4, 0x6b, 0xd2, 0x5b, 0xf5, 0xf0, 0x59, 0x5b, 0xbe,
    0x24, 0x65, 0x51, 0x41, 0x43, 0x8e, 0x7a, 0x10, 0x0b,
};

TEST(vendor_ed25519, RFC8032Test1_EmptyMessage_Verifies) {
    int ok = ed25519_verify(sig, NULL, 0, pubkey);
    TEST_ASSERT_EQUAL_INT(1, ok);
}

TEST(vendor_ed25519, RFC8032Test1_TamperedSignature_Rejected) {
    unsigned char bad[64];
    memcpy(bad, sig, sizeof(bad));
    bad[0] ^= 0x01;
    int ok = ed25519_verify(bad, NULL, 0, pubkey);
    TEST_ASSERT_EQUAL_INT(0, ok);
}

TEST(vendor_ed25519, RFC8032Test1_TamperedPubkey_Rejected) {
    unsigned char badkey[32];
    memcpy(badkey, pubkey, sizeof(badkey));
    badkey[0] ^= 0x01;
    int ok = ed25519_verify(sig, NULL, 0, badkey);
    TEST_ASSERT_EQUAL_INT(0, ok);
}

TEST(vendor_ed25519, RFC8032Test1_WrongMessage_Rejected) {
    static const unsigned char msg[] = {'x'};
    int ok = ed25519_verify(sig, msg, sizeof(msg), pubkey);
    TEST_ASSERT_EQUAL_INT(0, ok);
}

TEST_GROUP_RUNNER(vendor_ed25519) {
    RUN_TEST_CASE(vendor_ed25519, RFC8032Test1_EmptyMessage_Verifies);
    RUN_TEST_CASE(vendor_ed25519, RFC8032Test1_TamperedSignature_Rejected);
    RUN_TEST_CASE(vendor_ed25519, RFC8032Test1_TamperedPubkey_Rejected);
    RUN_TEST_CASE(vendor_ed25519, RFC8032Test1_WrongMessage_Rejected);
}
