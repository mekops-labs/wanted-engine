/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "test-utils.h"

#include <platform.h>
#include <wanted-api.h>
#include <wanted-image-verify.h>

#include "dummy-fs.h"

/* image_verify — the registry load check, driven against the in-memory
 * registry with hand-built metadata records. */

TEST_GROUP(image_verify);

static const uint8_t LAYER_A[] = "the base layer";
static const uint8_t LAYER_B[] = "the topmost layer";

TEST_SETUP(image_verify) { DummyRegistryReset(); }

TEST_TEAR_DOWN(image_verify) {}

static void Sha256(const uint8_t *data, size_t len,
                   uint8_t out[REGISTRY_META_DIGEST_LEN]) {
    void *ctx = PlatformSha256New();
    TEST_ASSERT_NOT_NULL(ctx);
    PlatformSha256Update(ctx, data, len);
    PlatformSha256Final(ctx, out);
    PlatformSha256Free(ctx);
}

/* Seed one image and return its entry. */
static reg_entry_t SeedImage(void) {
    reg_entry_t e = MakeEntry("app1", "1.0.0", 64);
    TEST_ASSERT_EQUAL_INT(1, DummyRegistrySeed(&e, 1));
    return e;
}

/* A wapp holding `count` mapped layers, topmost first. */
static wapp_t MakeWapp(const uint8_t *top, size_t topLen, const uint8_t *base,
                       size_t baseLen) {
    wapp_t w;
    memset(&w, 0, sizeof(w));
    w.layers[0] = (uint8_t *)top;
    w.layer_lens[0] = topLen;
    w.layer_cnt = 1;
    if (base != NULL) {
        w.layers[1] = (uint8_t *)base;
        w.layer_lens[1] = baseLen;
        w.layer_cnt = 2;
    }
    return w;
}

static registry_meta_t MakeMeta(uint8_t layers) {
    registry_meta_t m;
    memset(&m, 0, sizeof(m));
    m.magic = REGISTRY_META_MAGIC;
    m.layerCount = layers;
    return m;
}

TEST(image_verify, NoRecord_ForAnUnknownImage) {
    reg_entry_t ghost = MakeEntry("ghost", "9.9.9", 1);
    wapp_t w = MakeWapp(LAYER_A, sizeof(LAYER_A), NULL, 0);

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_NO_RECORD,
                          WantedVerifyImage(&ghost, &w));
}

TEST(image_verify, SeededImage_IsExemptEvenWhenTheDigestDisagrees) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_A, sizeof(LAYER_A), NULL, 0);

    memset(m.layerDigest[0], 0xff, REGISTRY_META_DIGEST_LEN);
    m.flags = REGISTRY_META_SEEDED;
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_SEEDED, WantedVerifyImage(&e, &w));
}

TEST(image_verify, MatchingDigest_WithoutASignature) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_A, sizeof(LAYER_A), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_NO_SIGNATURE, WantedVerifyImage(&e, &w));
}

TEST(image_verify, ChangedBytes_AreADigestMismatch) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_B, sizeof(LAYER_B), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_DIGEST_MISMATCH,
                          WantedVerifyImage(&e, &w));
}

/* The record runs base-first and the mapped layers run topmost-first. A
 * verifier reading either in its own natural order disagrees only here. */
TEST(image_verify, LayerOrder_RecordIsBaseFirst) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(2);
    wapp_t w = MakeWapp(LAYER_B, sizeof(LAYER_B), LAYER_A, sizeof(LAYER_A));

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    Sha256(LAYER_B, sizeof(LAYER_B), m.layerDigest[1]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_NO_SIGNATURE, WantedVerifyImage(&e, &w));
}

TEST(image_verify, LayerOrder_ReversedRecordIsRefused) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(2);
    wapp_t w = MakeWapp(LAYER_B, sizeof(LAYER_B), LAYER_A, sizeof(LAYER_A));

    Sha256(LAYER_B, sizeof(LAYER_B), m.layerDigest[0]);
    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[1]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_DIGEST_MISMATCH,
                          WantedVerifyImage(&e, &w));
}

TEST(image_verify, LayerCountMismatch_IsRefused) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(2);
    wapp_t w = MakeWapp(LAYER_A, sizeof(LAYER_A), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    Sha256(LAYER_B, sizeof(LAYER_B), m.layerDigest[1]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_DIGEST_MISMATCH,
                          WantedVerifyImage(&e, &w));
}

/* This build carries no keyring, so a signature naming any key id is under a
 * key the firmware does not hold. */
TEST(image_verify, SignatureUnderAnUnheldKey_IsRefused) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_A, sizeof(LAYER_A), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    memset(m.signature, 0x5a, sizeof(m.signature));
    m.keyId = 7;
    m.flags = REGISTRY_META_SIGNED;
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_UNKNOWN_KEY, WantedVerifyImage(&e, &w));
}

TEST(image_verify, EveryStateReportsItsOwnNameAndErrno) {
    const image_verify_state_t states[] = {
        IMAGE_VERIFY_OK,           IMAGE_VERIFY_SEEDED,
        IMAGE_VERIFY_NO_RECORD,    IMAGE_VERIFY_DIGEST_MISMATCH,
        IMAGE_VERIFY_NO_SIGNATURE, IMAGE_VERIFY_UNKNOWN_KEY,
        IMAGE_VERIFY_BAD_SIGNATURE};
    const size_t n = sizeof(states) / sizeof(states[0]);

    for (size_t i = 0; i < n; i++) {
        const char *ni = WantedImageVerifyStateName(states[i]);
        TEST_ASSERT_NOT_NULL(ni);
        for (size_t j = i + 1; j < n; j++) {
            TEST_ASSERT_TRUE(
                strcmp(ni, WantedImageVerifyStateName(states[j])) != 0);
            /* Only the two loading states share an errno, and it is 0. */
            int a = WantedImageVerifyErrno(states[i]);
            int b = WantedImageVerifyErrno(states[j]);
            if (a != 0 || b != 0)
                TEST_ASSERT_TRUE(a != b);
        }
    }
    TEST_ASSERT_EQUAL_INT(0, WantedImageVerifyErrno(IMAGE_VERIFY_OK));
    TEST_ASSERT_EQUAL_INT(0, WantedImageVerifyErrno(IMAGE_VERIFY_SEEDED));
}

TEST(image_verify, UnenforcedBuild_ReportsAndLoads) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_B, sizeof(LAYER_B), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    TEST_ASSERT_FALSE(WantedImageVerifyFloor());
    TEST_ASSERT_EQUAL_INT(IMAGE_VERIFY_DIGEST_MISMATCH,
                          WantedVerifyImage(&e, &w));
    TEST_ASSERT_EQUAL_INT(0, WantedImageVerifyGate(&e, &w));
}

/* Raising is process-wide and one-way, so this runs last in the group. */
TEST(image_verify, EnforcementRisesAndNeverFalls) {
    reg_entry_t e = SeedImage();
    registry_meta_t m = MakeMeta(1);
    wapp_t w = MakeWapp(LAYER_B, sizeof(LAYER_B), NULL, 0);

    Sha256(LAYER_A, sizeof(LAYER_A), m.layerDigest[0]);
    TEST_ASSERT_EQUAL_INT(0, DummyRegistrySetMeta(&e, &m));

    WantedImageVerifyRaise(false);
    TEST_ASSERT_FALSE(WantedImageVerifyEnforced());

    WantedImageVerifyRaise(true);
    TEST_ASSERT_TRUE(WantedImageVerifyEnforced());
    TEST_ASSERT_EQUAL_INT(-EBADMSG, WantedImageVerifyGate(&e, &w));

    /* A later source asking for it off leaves it on. */
    WantedImageVerifyRaise(false);
    TEST_ASSERT_TRUE(WantedImageVerifyEnforced());
    /* The floor is what a configuration cannot reach. */
    TEST_ASSERT_FALSE(WantedImageVerifyFloor());
}

/* Fixed vectors shared with the signer, so a message this build assembles is
 * byte-identical to the one that was signed. */
static void AssertMessage(const char *name, const char *version,
                          const char *const *digests, uint8_t count,
                          const char *expectHex) {
    reg_entry_t e = MakeEntry(name, version, 0);
    registry_meta_t m = MakeMeta(count);
    uint8_t msg[256];
    char hex[sizeof(msg) * 2 + 1];

    for (uint8_t i = 0; i < count; i++) {
        for (size_t b = 0; b < REGISTRY_META_DIGEST_LEN; b++) {
            unsigned v = 0;
            sscanf(digests[i] + b * 2, "%2x", &v);
            m.layerDigest[i][b] = (uint8_t)v;
        }
    }

    size_t n = WantedImageSignedMessage(&e, &m, msg, sizeof(msg));
    TEST_ASSERT_TRUE(n > 0);
    for (size_t i = 0; i < n; i++)
        snprintf(hex + i * 2, 3, "%02x", msg[i]);
    hex[n * 2] = '\0';
    TEST_ASSERT_EQUAL_STRING(expectHex, hex);
}

TEST(image_verify, SignedMessage_MatchesTheSingleLayerVector) {
    static const char *const digests[] = {
        "1111111111111111111111111111111111111111111111111111111111111111"};

    AssertMessage("sheriff", "0.8.0", digests, 1,
                  "077368657269666605302e382e3001"
                  "1111111111111111111111111111111111111111111111111111111111"
                  "111111");
}

TEST(image_verify, SignedMessage_MatchesTheMultiLayerVector) {
    static const char *const digests[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "1111111111111111111111111111111111111111111111111111111111111111",
        "2222222222222222222222222222222222222222222222222222222222222222"};

    AssertMessage("tg-display", "1.2.3-rc1", digests, 3,
                  "0a74672d646973706c617909312e322e332d72633103"
                  "0000000000000000000000000000000000000000000000000000000000"
                  "000000"
                  "1111111111111111111111111111111111111111111111111111111111"
                  "111111"
                  "2222222222222222222222222222222222222222222222222222222222"
                  "222222");
}

/* Length prefixes are what separate these two identities: without them both
 * assemble to the same bytes. */
TEST(image_verify, SignedMessage_LengthPrefixesSeparateTheAmbiguousPair) {
    reg_entry_t a = MakeEntry("foo", "1.0", 0);
    reg_entry_t b = MakeEntry("foo1", ".0", 0);
    registry_meta_t m = MakeMeta(1);
    uint8_t ma[128], mb[128];

    memset(m.layerDigest[0], 0x11, REGISTRY_META_DIGEST_LEN);
    size_t na = WantedImageSignedMessage(&a, &m, ma, sizeof(ma));
    size_t nb = WantedImageSignedMessage(&b, &m, mb, sizeof(mb));

    TEST_ASSERT_EQUAL_size_t(na, nb);
    TEST_ASSERT_TRUE(memcmp(ma, mb, na) != 0);
}

TEST(image_verify, SignedMessage_RefusesAShortBuffer) {
    reg_entry_t e = MakeEntry("sheriff", "0.8.0", 0);
    registry_meta_t m = MakeMeta(1);
    uint8_t msg[8];

    TEST_ASSERT_EQUAL_size_t(
        0, WantedImageSignedMessage(&e, &m, msg, sizeof(msg)));
}

TEST_GROUP_RUNNER(image_verify) {
    RUN_TEST_CASE(image_verify, NoRecord_ForAnUnknownImage);
    RUN_TEST_CASE(image_verify, SeededImage_IsExemptEvenWhenTheDigestDisagrees);
    RUN_TEST_CASE(image_verify, MatchingDigest_WithoutASignature);
    RUN_TEST_CASE(image_verify, ChangedBytes_AreADigestMismatch);
    RUN_TEST_CASE(image_verify, LayerOrder_RecordIsBaseFirst);
    RUN_TEST_CASE(image_verify, LayerOrder_ReversedRecordIsRefused);
    RUN_TEST_CASE(image_verify, LayerCountMismatch_IsRefused);
    RUN_TEST_CASE(image_verify, SignatureUnderAnUnheldKey_IsRefused);
    RUN_TEST_CASE(image_verify, EveryStateReportsItsOwnNameAndErrno);
    RUN_TEST_CASE(image_verify, UnenforcedBuild_ReportsAndLoads);
    RUN_TEST_CASE(image_verify, SignedMessage_MatchesTheSingleLayerVector);
    RUN_TEST_CASE(image_verify, SignedMessage_MatchesTheMultiLayerVector);
    RUN_TEST_CASE(image_verify,
                  SignedMessage_LengthPrefixesSeparateTheAmbiguousPair);
    RUN_TEST_CASE(image_verify, SignedMessage_RefusesAShortBuffer);
    RUN_TEST_CASE(image_verify, EnforcementRisesAndNeverFalls);
}
