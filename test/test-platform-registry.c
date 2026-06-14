/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * platform_registry — in-memory registry store exercised via PlatformRegistry*
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(platform_registry);

TEST_SETUP(platform_registry)    { DummyRegistryReset(); }
TEST_TEAR_DOWN(platform_registry) {}

TEST(platform_registry, ReadEmpty_ReturnsZero) {
    reg_entry_t list[4];
    TEST_ASSERT_EQUAL_INT(0, PlatformRegistryRead(list, 4));
}

TEST(platform_registry, ReadCountQuery_NullListReturnsCount) {
    reg_entry_t seed[2] = {
        MakeEntry("app1", "1.0.0", 10),
        MakeEntry("app2", "2.1.0", 20),
    };
    TEST_ASSERT_EQUAL_INT(2, DummyRegistrySeed(seed, 2));
    TEST_ASSERT_EQUAL_INT(2, PlatformRegistryRead(NULL, 0));
}

TEST(platform_registry, SeedThenRead_RoundTrip) {
    reg_entry_t seed[2] = {
        MakeEntry("sensor", "0.3.1", 128),
        MakeEntry("relay",  "1.2.0", 256),
    };
    TEST_ASSERT_EQUAL_INT(2, DummyRegistrySeed(seed, 2));

    reg_entry_t list[4];
    int n = PlatformRegistryRead(list, 4);
    TEST_ASSERT_EQUAL_INT(2, n);

    /* Insertion order is preserved by the linear scan. */
    TEST_ASSERT_EQUAL_STRING("sensor", list[0].name);
    TEST_ASSERT_EQUAL_STRING("0.3.1", list[0].version);
    TEST_ASSERT_EQUAL_UINT32(128, list[0].size);
    TEST_ASSERT_EQUAL_STRING("relay", list[1].name);
    TEST_ASSERT_EQUAL_STRING("1.2.0", list[1].version);
    TEST_ASSERT_EQUAL_UINT32(256, list[1].size);
}

TEST(platform_registry, UpsertByName_ReplacesVersion) {
    reg_entry_t first  = MakeEntry("app", "1.0.0", 10);
    reg_entry_t second = MakeEntry("app", "1.1.0", 99);
    TEST_ASSERT_EQUAL_INT(1, DummyRegistrySeed(&first, 1));
    TEST_ASSERT_EQUAL_INT(1, DummyRegistrySeed(&second, 1));

    reg_entry_t list[4];
    int n = PlatformRegistryRead(list, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("app", list[0].name);
    TEST_ASSERT_EQUAL_STRING("1.1.0", list[0].version);
    TEST_ASSERT_EQUAL_UINT32(99, list[0].size);
}

TEST(platform_registry, Read_CapsFillButReturnsTotal) {
    reg_entry_t seed[3] = {
        MakeEntry("a", "1", 1),
        MakeEntry("b", "1", 2),
        MakeEntry("c", "1", 3),
    };
    TEST_ASSERT_EQUAL_INT(3, DummyRegistrySeed(seed, 3));

    reg_entry_t list[2];
    /* len smaller than total: fills 2, still reports the full count of 3. */
    TEST_ASSERT_EQUAL_INT(3, PlatformRegistryRead(list, 2));
    TEST_ASSERT_EQUAL_STRING("a", list[0].name);
    TEST_ASSERT_EQUAL_STRING("b", list[1].name);
}

TEST(platform_registry, Remove_DeletesEntry) {
    reg_entry_t seed[2] = {
        MakeEntry("keep", "1.0.0", 10),
        MakeEntry("drop", "1.0.0", 20),
    };
    DummyRegistrySeed(seed, 2);

    reg_entry_t target = MakeEntry("drop", "", 0);
    TEST_ASSERT_EQUAL_INT(0, PlatformRegistryRemove(&target));

    reg_entry_t list[4];
    int n = PlatformRegistryRead(list, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_STRING("keep", list[0].name);
}

TEST(platform_registry, Remove_Nonexistent_ReturnsEnoent) {
    reg_entry_t target = MakeEntry("ghost", "", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, PlatformRegistryRemove(&target));
}

TEST(platform_registry, Overfill_ReturnsEnospc) {
    /* The table holds 8 entries; seeding 9 distinct names must report -ENOSPC. */
    reg_entry_t seed[9];
    for (int i = 0; i < 9; i++) {
        char name[WAPP_MAX_NAME_LEN];
        snprintf(name, sizeof(name), "ovf%d", i);
        seed[i] = MakeEntry(name, "1.0.0", (size_t)i);
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, DummyRegistrySeed(seed, 9));
}

TEST_GROUP_RUNNER(platform_registry) {
    RUN_TEST_CASE(platform_registry, ReadEmpty_ReturnsZero);
    RUN_TEST_CASE(platform_registry, ReadCountQuery_NullListReturnsCount);
    RUN_TEST_CASE(platform_registry, SeedThenRead_RoundTrip);
    RUN_TEST_CASE(platform_registry, UpsertByName_ReplacesVersion);
    RUN_TEST_CASE(platform_registry, Read_CapsFillButReturnsTotal);
    RUN_TEST_CASE(platform_registry, Remove_DeletesEntry);
    RUN_TEST_CASE(platform_registry, Remove_Nonexistent_ReturnsEnoent);
    RUN_TEST_CASE(platform_registry, Overfill_ReturnsEnospc);
}

