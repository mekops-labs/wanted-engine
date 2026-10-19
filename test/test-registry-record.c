/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <string.h>

#include <registry-meta.h>

/* registry_record — the stored form of an entry record. */

TEST_GROUP(registry_record);

TEST_SETUP(registry_record) {}
TEST_TEAR_DOWN(registry_record) {}

static registry_meta_t sample(void) {
    registry_meta_t m;

    memset(&m, 0, sizeof(m));
    m.magic = REGISTRY_META_MAGIC;
    m.layerCount = 2;
    m.flags = REGISTRY_META_SIGNED | REGISTRY_META_CLOCK_UNSET;
    m.keyId = 0x01020304u;
    m.installSeq = 77;
    m.installed = 1700000000;
    m.created = -5;
    m.size = 300;
    m.layerLen[0] = 100;
    m.layerLen[1] = 200;
    memset(m.layerDigest[0], 0xa1, REGISTRY_META_DIGEST_LEN);
    memset(m.layerDigest[1], 0xb2, REGISTRY_META_DIGEST_LEN);
    memset(m.signature, 0xc3, REGISTRY_META_SIG_LEN);
    return m;
}

TEST(registry_record, RoundTrip_KeepsEveryField) {
    registry_meta_t in = sample();
    registry_meta_t out;
    uint8_t buf[REGISTRY_RECORD_MAX];
    size_t n = RegistryRecordEncode(&in, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT32(REGISTRY_RECORD_HEADER_LEN +
                                 2 * REGISTRY_RECORD_LAYER_LEN +
                                 REGISTRY_META_SIG_LEN,
                             n);
    TEST_ASSERT_TRUE(RegistryRecordDecode(buf, n, &out));
    TEST_ASSERT_EQUAL_UINT32(in.magic, out.magic);
    TEST_ASSERT_EQUAL_UINT32(in.size, out.size);
    TEST_ASSERT_EQUAL_UINT32(in.keyId, out.keyId);
    TEST_ASSERT_EQUAL_UINT32(in.installSeq, out.installSeq);
    TEST_ASSERT_EQUAL_INT64(in.installed, out.installed);
    TEST_ASSERT_EQUAL_INT64(in.created, out.created);
    TEST_ASSERT_EQUAL_UINT8(in.layerCount, out.layerCount);
    TEST_ASSERT_EQUAL_UINT8(in.flags, out.flags);
    TEST_ASSERT_EQUAL_UINT32(100, out.layerLen[0]);
    TEST_ASSERT_EQUAL_UINT32(200, out.layerLen[1]);
    TEST_ASSERT_EQUAL_MEMORY(in.layerDigest[1], out.layerDigest[1],
                             REGISTRY_META_DIGEST_LEN);
    TEST_ASSERT_EQUAL_MEMORY(in.signature, out.signature,
                             REGISTRY_META_SIG_LEN);
}

TEST(registry_record, Encode_FixesByteOrder) {
    registry_meta_t in = sample();
    uint8_t buf[REGISTRY_RECORD_MAX];

    TEST_ASSERT_TRUE(RegistryRecordEncode(&in, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_UINT8('4', buf[0]);
    TEST_ASSERT_EQUAL_UINT8('P', buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x04, buf[8]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[11]);
    TEST_ASSERT_EQUAL_UINT8(77, buf[12]);
}

TEST(registry_record, Encode_RefusesASmallBufferAndTooManyLayers) {
    registry_meta_t in = sample();
    uint8_t buf[REGISTRY_RECORD_MAX];

    TEST_ASSERT_EQUAL_UINT32(0, RegistryRecordEncode(&in, buf, 20));
    in.layerCount = REGISTRY_META_MAX_LAYERS + 1;
    TEST_ASSERT_EQUAL_UINT32(0, RegistryRecordEncode(&in, buf, sizeof(buf)));
}

TEST(registry_record, Decode_RefusesShortUnknownAndOverlongRecords) {
    registry_meta_t in = sample();
    registry_meta_t out;
    uint8_t buf[REGISTRY_RECORD_MAX];
    size_t n = RegistryRecordEncode(&in, buf, sizeof(buf));

    TEST_ASSERT_FALSE(RegistryRecordDecode(buf, n - 1, &out));
    TEST_ASSERT_FALSE(RegistryRecordDecode(buf, 3, &out));
    buf[0] ^= 0xff;
    TEST_ASSERT_FALSE(RegistryRecordDecode(buf, n, &out));
    buf[0] ^= 0xff;
    buf[5] = REGISTRY_META_MAX_LAYERS + 1;
    TEST_ASSERT_FALSE(RegistryRecordDecode(buf, sizeof(buf), &out));
}

/* A WAP3 record: magic, size, key id, layer count, flags, two pad bytes, a
 * digest for each of the build's layer slots, then the signature. */
TEST(registry_record, Decode_ReadsAnOldRecordAsOneLayerWithNoTimes) {
    uint8_t old[16 + REGISTRY_META_MAX_LAYERS * REGISTRY_META_DIGEST_LEN +
                REGISTRY_META_SIG_LEN];
    registry_meta_t out;

    memset(old, 0, sizeof(old));
    old[0] = '3';
    old[1] = 'P';
    old[2] = 'A';
    old[3] = 'W';
    old[4] = 0x90;
    old[5] = 0x01;
    old[8] = 0x07;
    old[12] = 1;
    old[13] = REGISTRY_META_SIGNED;
    memset(old + 16, 0xd4, REGISTRY_META_DIGEST_LEN);
    memset(old + sizeof(old) - REGISTRY_META_SIG_LEN, 0xe5,
           REGISTRY_META_SIG_LEN);

    TEST_ASSERT_TRUE(RegistryRecordDecode(old, sizeof(old), &out));
    TEST_ASSERT_EQUAL_UINT32(REGISTRY_META_MAGIC, out.magic);
    TEST_ASSERT_EQUAL_UINT32(0x190, out.size);
    TEST_ASSERT_EQUAL_UINT32(0x190, out.layerLen[0]);
    TEST_ASSERT_EQUAL_UINT32(7, out.keyId);
    TEST_ASSERT_EQUAL_UINT8(1, out.layerCount);
    TEST_ASSERT_EQUAL_UINT32(0, out.installSeq);
    TEST_ASSERT_EQUAL_INT64(0, out.installed);
    TEST_ASSERT_TRUE((out.flags & REGISTRY_META_SIGNED) != 0);
    TEST_ASSERT_TRUE((out.flags & REGISTRY_META_CLOCK_UNSET) != 0);
    TEST_ASSERT_EQUAL_UINT8(0xd4, out.layerDigest[0][5]);
    TEST_ASSERT_EQUAL_UINT8(0xe5, out.signature[63]);
    TEST_ASSERT_FALSE(RegistryRecordDecode(old, sizeof(old) - 1, &out));
}

TEST_GROUP_RUNNER(registry_record) {
    RUN_TEST_CASE(registry_record, RoundTrip_KeepsEveryField);
    RUN_TEST_CASE(registry_record, Encode_FixesByteOrder);
    RUN_TEST_CASE(registry_record, Encode_RefusesASmallBufferAndTooManyLayers);
    RUN_TEST_CASE(registry_record,
                  Decode_RefusesShortUnknownAndOverlongRecords);
    RUN_TEST_CASE(registry_record,
                  Decode_ReadsAnOldRecordAsOneLayerWithNoTimes);
}
