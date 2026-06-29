/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <wanted-vfs-api.h>

/* Exercises WantedWasmMemoryProfile (wanted-vfs-api.c) — the standalone wasm
 * (memory) section parser behind the registry descriptor's memory metadata. */

#define MAGIC 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00

TEST_GROUP(wasm_meta);
TEST_SETUP(wasm_meta) {}
TEST_TEAR_DOWN(wasm_meta) {}

TEST(wasm_meta, MinOnly) {
    /* (memory 1): section 5, count 1, flags 0, min 1. */
    const uint8_t m[] = {MAGIC, 0x05, 0x03, 0x01, 0x00, 0x01};
    uint32_t init = 0, max = 0xdead;
    bool has_max = true;
    TEST_ASSERT_EQUAL_INT(0, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                     &has_max, &max));
    TEST_ASSERT_EQUAL_UINT32(1, init);
    TEST_ASSERT_FALSE(has_max);
}

TEST(wasm_meta, MinAndMax) {
    /* (memory 1 8): flags 1, min 1, max 8. */
    const uint8_t m[] = {MAGIC, 0x05, 0x04, 0x01, 0x01, 0x01, 0x08};
    uint32_t init = 0, max = 0;
    bool has_max = false;
    TEST_ASSERT_EQUAL_INT(0, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                     &has_max, &max));
    TEST_ASSERT_EQUAL_UINT32(1, init);
    TEST_ASSERT_TRUE(has_max);
    TEST_ASSERT_EQUAL_UINT32(8, max);
}

TEST(wasm_meta, SkipsEarlierSections) {
    /* A (type) section (id 1, empty) precedes the memory section (min 2). */
    const uint8_t m[] = {MAGIC, 0x01, 0x01, 0x00, 0x05, 0x03, 0x01, 0x00, 0x02};
    uint32_t init = 0, max = 0;
    bool has_max = true;
    TEST_ASSERT_EQUAL_INT(0, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                     &has_max, &max));
    TEST_ASSERT_EQUAL_UINT32(2, init);
    TEST_ASSERT_FALSE(has_max);
}

TEST(wasm_meta, MultiByteLeb) {
    /* (memory 128): min encoded as uLEB128 0x80 0x01. */
    const uint8_t m[] = {MAGIC, 0x05, 0x04, 0x01, 0x00, 0x80, 0x01};
    uint32_t init = 0, max = 0;
    bool has_max = true;
    TEST_ASSERT_EQUAL_INT(0, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                     &has_max, &max));
    TEST_ASSERT_EQUAL_UINT32(128, init);
}

TEST(wasm_meta, NoMemorySection) {
    /* Only a (type) section, no memory. */
    const uint8_t m[] = {MAGIC, 0x01, 0x01, 0x00};
    uint32_t init = 0, max = 0;
    bool has_max = false;
    TEST_ASSERT_EQUAL_INT(-ENOENT, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                           &has_max, &max));
}

TEST(wasm_meta, BadMagic) {
    const uint8_t m[] = {0, 0, 0, 0, 1, 0, 0, 0, 0x05, 0x03, 0x01, 0x00, 0x01};
    uint32_t init = 0, max = 0;
    bool has_max = false;
    TEST_ASSERT_EQUAL_INT(-EINVAL, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                          &has_max, &max));
}

TEST(wasm_meta, TruncatedMemorySection) {
    /* Section 5 declares length 3 but only one payload byte follows. */
    const uint8_t m[] = {MAGIC, 0x05, 0x03, 0x01};
    uint32_t init = 0, max = 0;
    bool has_max = false;
    TEST_ASSERT_EQUAL_INT(-EINVAL, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                          &has_max, &max));
}

TEST(wasm_meta, ShortWindowBeforeMemoryIsAbsent) {
    /* A large (function) section (id 3) the window stops short of — the parser
     * can't reach the memory section, so it reports absent, not malformed. */
    const uint8_t m[] = {MAGIC, 0x03, 0x40, 0x00, 0x00};
    uint32_t init = 0, max = 0;
    bool has_max = false;
    TEST_ASSERT_EQUAL_INT(-ENOENT, WantedWasmMemoryProfile(m, sizeof(m), &init,
                                                          &has_max, &max));
}

TEST_GROUP_RUNNER(wasm_meta) {
    RUN_TEST_CASE(wasm_meta, MinOnly);
    RUN_TEST_CASE(wasm_meta, MinAndMax);
    RUN_TEST_CASE(wasm_meta, SkipsEarlierSections);
    RUN_TEST_CASE(wasm_meta, MultiByteLeb);
    RUN_TEST_CASE(wasm_meta, NoMemorySection);
    RUN_TEST_CASE(wasm_meta, BadMagic);
    RUN_TEST_CASE(wasm_meta, TruncatedMemorySection);
    RUN_TEST_CASE(wasm_meta, ShortWindowBeforeMemoryIsAbsent);
}
