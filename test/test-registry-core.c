/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <registry-mem.h>
#include <registry.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* registry_core — the registry rules above the store interface, run against
 * the in-memory store. */

TEST_GROUP(registry_core);

static wapp_t wapp;

TEST_SETUP(registry_core) {
    DummyRegistryReset();
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
    memset(&wapp, 0, sizeof(wapp));
}

TEST_TEAR_DOWN(registry_core) { DummyRegistryReset(); }

static int stageLayer(const char *ref, unsigned layer, const char *bytes) {
    size_t n = strlen(bytes);
    int rc =
        RegistryWriteLayer(START_WRITE, ref, layer, (const uint8_t *)bytes, n);

    if (rc < 0)
        return rc;
    return RegistryWriteLayer(FINISH_WRITE, NULL, 0, NULL, 0);
}

static int installPlain(const char *ref, const char *bytes) {
    size_t n = strlen(bytes);
    int rc = RegistryWrite(START_WRITE, ref, (const uint8_t *)bytes, n);

    if (rc < 0)
        return rc;
    return RegistryWrite(FINISH_WRITE, NULL, NULL, 0);
}

static reg_entry_t entryOf(const char *name, const char *version) {
    return MakeEntry(name, version, 0);
}

static int blobCount(void) {
    registry_store_t *s = RegistryMemStore();
    uint8_t digests[REGISTRY_RECORD_ABS_MAX_LAYERS * 4][REGISTRY_DIGEST_LEN];

    return s->ops->blobList(s, digests, 0);
}

static void sha(const char *bytes, uint8_t out[REGISTRY_DIGEST_LEN]) {
    void *ctx = PlatformSha256New();

    PlatformSha256Update(ctx, (const uint8_t *)bytes, strlen(bytes));
    PlatformSha256Final(ctx, out);
    PlatformSha256Free(ctx);
}

TEST(registry_core, PlainInstall_ListsOneLayerEntry) {
    reg_entry_t list[4];
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;
    uint8_t digest[REGISTRY_DIGEST_LEN];

    TEST_ASSERT_EQUAL_INT(5, RegistryWrite(START_WRITE, "app:1.0.0",
                                           (const uint8_t *)"hello", 5));
    TEST_ASSERT_EQUAL_INT(0, RegistryWrite(FINISH_WRITE, NULL, NULL, 0));

    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_STRING("app", list[0].name);
    TEST_ASSERT_EQUAL_STRING("1.0.0", list[0].version);
    TEST_ASSERT_EQUAL_UINT32(5, list[0].size);

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT8(1, meta.layerCount);
    TEST_ASSERT_EQUAL_UINT32(5, meta.layerLen[0]);
    sha("hello", digest);
    TEST_ASSERT_EQUAL_MEMORY(digest, meta.layerDigest[0], sizeof(digest));
    TEST_ASSERT_EQUAL_INT(0, meta.flags & REGISTRY_META_SIGNED);
}

TEST(registry_core, PlainInstall_Replaces_FreesTheOldBlob) {
    reg_entry_t list[4];

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "aaa"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "bbbb"));
    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_UINT32(4, list[0].size);
    TEST_ASSERT_EQUAL_INT(1, blobCount());
}

TEST(registry_core, StagedLayers_AreHiddenUntilCommit) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 1, "top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryLoad(&e, &wapp));

    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(1, RegistryRead(NULL, 0));
}

TEST(registry_core, Layers_ArriveInAnyOrder_CommitKeepsIndexOrder) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;
    uint8_t base[REGISTRY_DIGEST_LEN];
    uint8_t top[REGISTRY_DIGEST_LEN];

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 1, "top"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 2, false, 0));

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT8(2, meta.layerCount);
    sha("base", base);
    sha("top", top);
    TEST_ASSERT_EQUAL_MEMORY(base, meta.layerDigest[0], sizeof(base));
    TEST_ASSERT_EQUAL_MEMORY(top, meta.layerDigest[1], sizeof(top));
    TEST_ASSERT_EQUAL_UINT32(7, meta.size);
}

TEST(registry_core, Commit_MissingLayer_FailsAndShowsNothing) {
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryCommit("app:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
}

TEST(registry_core, Commit_CountOutOfRange_IsRefused) {
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RegistryCommit("app:1.0.0", 0, false, 0));
    TEST_ASSERT_EQUAL_INT(
        -E2BIG,
        RegistryCommit("app:1.0.0", REGISTRY_META_MAX_LAYERS + 1, false, 0));
}

TEST(registry_core, LayerIndex_AtTheLimit_IsRefused) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, RegistryWriteLayer(START_WRITE, "app:1.0.0",
                                                      REGISTRY_META_MAX_LAYERS,
                                                      (const uint8_t *)"x", 1));
}

TEST(registry_core, Commit_RecordsTheBuildTime) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "x"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 1, true, 1700000123));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT64(1700000123, meta.created);

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "y"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 1, false, 99));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT64(0, meta.created);
}

TEST(registry_core, Commit_StampsTheInstallTimeWhenTheClockIsValid) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0,
                          PlatformClockSetTime(PLAT_CLOCKID_REALTIME,
                                               1700000000ULL * 1000000000ULL));
    WantedSetClockQuality(WANTED_CLOCK_HARDWARE_RTC);
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "x"));

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_TRUE(meta.installed >= 1700000000);
    TEST_ASSERT_TRUE(meta.installed < 1700000100);
    TEST_ASSERT_EQUAL_INT(0, meta.flags & REGISTRY_META_CLOCK_UNSET);
}

TEST(registry_core, Commit_WithAnUnsetClock_FlagsTheRecord) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "x"));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT64(0, meta.installed);
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_CLOCK_UNSET) != 0);
}

TEST(registry_core, Resend_ReplacesTheStagedLayer) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;
    uint8_t digest[REGISTRY_DIGEST_LEN];

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "aaa"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "bbb"));
    TEST_ASSERT_EQUAL_INT(1, blobCount());
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 1, false, 0));

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    sha("bbb", digest);
    TEST_ASSERT_EQUAL_MEMORY(digest, meta.layerDigest[0], sizeof(digest));
}

TEST(registry_core, Commit_DropsLayersAboveTheCount) {
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 1, "extra"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 1, false, 0));
    TEST_ASSERT_EQUAL_INT(1, blobCount());
}

TEST(registry_core, SharedLayer_IsStoredOnce_AndSurvivesOneRemoval) {
    reg_entry_t a = entryOf("one", "1.0.0");
    reg_entry_t b = entryOf("two", "1.0.0");

    TEST_ASSERT_EQUAL_INT(0, stageLayer("one:1.0.0", 0, "shared"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("one:1.0.0", 1, "one-top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("one:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("two:1.0.0", 0, "shared"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("two:1.0.0", 1, "two-top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("two:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(3, blobCount());

    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&a));
    TEST_ASSERT_EQUAL_INT(2, blobCount());
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&b, &wapp));
    TEST_ASSERT_EQUAL_UINT8(2, wapp.layer_cnt);
    RegistryUnload(&wapp);
}

TEST(registry_core, Remove_FreesEveryBlobOfTheEntry) {
    reg_entry_t e = entryOf("app", "1.0.0");

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 1, "top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    TEST_ASSERT_EQUAL_INT(0, blobCount());
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryRemove(&e));
}

TEST(registry_core, MappedBlob_SurvivesRemoval_UntilTheWappUnloads) {
    reg_entry_t e = entryOf("app", "1.0.0");

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "image"));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));
    TEST_ASSERT_TRUE(RegistryIsMapped(&e));

    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    TEST_ASSERT_EQUAL_INT(1, blobCount());
    TEST_ASSERT_EQUAL_MEMORY("image", wapp.layers[0], 5);

    TEST_ASSERT_TRUE(RegistryUnload(&wapp));
    TEST_ASSERT_EQUAL_INT(0, blobCount());
}

TEST(registry_core, ReplaceWhileRunning_KeepsTheOldBlobMapped) {
    reg_entry_t e = entryOf("app", "1.0.0");

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "old-image"));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "new-image"));
    TEST_ASSERT_EQUAL_INT(2, blobCount());
    TEST_ASSERT_EQUAL_MEMORY("old-image", wapp.layers[0], 9);

    RegistryUnload(&wapp);
    TEST_ASSERT_EQUAL_INT(1, blobCount());
}

TEST(registry_core, Load_MapsLayersTopmostFirst) {
    reg_entry_t e = entryOf("app", "1.0.0");

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 1, "top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 2, false, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));

    TEST_ASSERT_EQUAL_UINT8(2, wapp.layer_cnt);
    TEST_ASSERT_EQUAL_MEMORY("top", wapp.layers[0], 3);
    TEST_ASSERT_EQUAL_UINT32(3, wapp.layer_lens[0]);
    TEST_ASSERT_EQUAL_MEMORY("base", wapp.layers[1], 4);
    TEST_ASSERT_EQUAL_STRING("app", wapp.image);
    TEST_ASSERT_EQUAL_STRING("1.0.0", wapp.version);
    RegistryUnload(&wapp);
}

TEST(registry_core, BareName_TakesTheNewestInstall_NotTheHighestVersion) {
    reg_entry_t bare = entryOf("app", "");

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.10.0", "ten"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.9.0", "nine"));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&bare, &wapp));
    TEST_ASSERT_EQUAL_STRING("0.9.0", wapp.version);
    RegistryUnload(&wapp);

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.10.0", "ten-again"));
    memset(&wapp, 0, sizeof(wapp));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&bare, &wapp));
    TEST_ASSERT_EQUAL_STRING("0.10.0", wapp.version);
    RegistryUnload(&wapp);
}

TEST(registry_core, BareName_ServesTheRecordOfTheEntryItResolves) {
    reg_entry_t bare = entryOf("app", "");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "one"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:2.0.0", "twotwo"));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&bare, &meta));
    TEST_ASSERT_EQUAL_UINT32(6, meta.size);
}

TEST(registry_core, InstallSequence_ContinuesAfterARestart) {
    reg_entry_t bare = entryOf("app", "");

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.10.0", "ten"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.9.0", "nine"));
    RegistryDeinit();
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:0.11.0", "eleven"));

    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&bare, &wapp));
    TEST_ASSERT_EQUAL_STRING("0.11.0", wapp.version);
    RegistryUnload(&wapp);
}

TEST(registry_core, SeededEntry_CannotBeRemoved) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "seed"));
    RegistryMarkSeeded("app:1.0.0");
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_SEEDED) != 0);
    TEST_ASSERT_EQUAL_INT(-EPERM, RegistryRemove(&e));
    TEST_ASSERT_EQUAL_INT(1, RegistryRead(NULL, 0));
}

TEST(registry_core, InstallOverASeed_ClearsTheSeededFlag) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "seed"));
    RegistryMarkSeeded("app:1.0.0");
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "newer"));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT(0, meta.flags & REGISTRY_META_SEEDED);
    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
}

TEST(registry_core, SecondInstall_WhileOneIsOpen_ReturnsEbusy) {
    TEST_ASSERT_EQUAL_INT(
        1, RegistryWrite(START_WRITE, "one:1", (const uint8_t *)"a", 1));
    TEST_ASSERT_EQUAL_INT(
        -EBUSY, RegistryWrite(START_WRITE, "two:1", (const uint8_t *)"b", 1));
    TEST_ASSERT_EQUAL_INT(0, RegistryWrite(FINISH_WRITE, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, installPlain("two:1", "b"));
}

TEST(registry_core, StreamArrivesInChunks_AndIsHashedWhole) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;
    uint8_t digest[REGISTRY_DIGEST_LEN];

    TEST_ASSERT_EQUAL_INT(
        3, RegistryWrite(START_WRITE, "app:1.0.0", (const uint8_t *)"abc", 3));
    TEST_ASSERT_EQUAL_INT(
        3, RegistryWrite(CONTINUE_WRITE, NULL, (const uint8_t *)"def", 3));
    TEST_ASSERT_EQUAL_INT(0, RegistryWrite(FINISH_WRITE, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    sha("abcdef", digest);
    TEST_ASSERT_EQUAL_MEMORY(digest, meta.layerDigest[0], sizeof(digest));
    TEST_ASSERT_EQUAL_UINT32(6, meta.size);
}

TEST(registry_core, StoreWithNoRoom_FailsTheStageAndLeavesNoneOpen) {
    RegistryMemSetLimit(4);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, installPlain("app:1.0.0", "toolong"));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, blobCount());

    RegistryMemSetLimit(0);
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "ok"));
}

TEST(registry_core, CutStream_FailingMidway_CommitsNothing) {
    TEST_ASSERT_EQUAL_INT(
        3, RegistryWrite(START_WRITE, "app:1.0.0", (const uint8_t *)"abc", 3));
    RegistryMemSetLimit(4);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, RegistryWrite(CONTINUE_WRITE, NULL,
                                                 (const uint8_t *)"defg", 4));
    TEST_ASSERT_EQUAL_INT(-ENOSPC, RegistryWrite(FINISH_WRITE, NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, blobCount());
}

TEST(registry_core, Restart_ReclaimsBlobsNoEntryLists) {
    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(1, blobCount());
    RegistryDeinit();
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, blobCount());
}

TEST(registry_core, Signature_OnAStagedRef_ReturnsEnoent) {
    reg_entry_t e = entryOf("app", "1.0.0");
    uint8_t sig[REGISTRY_META_SIG_LEN] = {1};

    TEST_ASSERT_EQUAL_INT(0, stageLayer("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryMetaSetSignature(&e, 1, sig));
}

TEST(registry_core, Commit_StartsUnsigned_AndASignatureLandsAfterIt) {
    reg_entry_t e = entryOf("app", "1.0.0");
    registry_meta_t meta;
    uint8_t sig[REGISTRY_META_SIG_LEN];

    memset(sig, 0x5a, sizeof(sig));
    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "x"));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaSetSignature(&e, 7, sig));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT32(7, meta.keyId);
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_SIGNED) != 0);

    TEST_ASSERT_EQUAL_INT(0, installPlain("app:1.0.0", "y"));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT(0, meta.flags & REGISTRY_META_SIGNED);
}

TEST(registry_core, Capacity_ReportsEachStore) {
    registry_capacity_t cap[2];

    TEST_ASSERT_EQUAL_INT(1, RegistryCapacity(cap, 2));
    TEST_ASSERT_EQUAL_STRING("mem", cap[0].kind);
    TEST_ASSERT_TRUE((cap[0].caps & REGISTRY_CAP_MAPPABLE) != 0);
    TEST_ASSERT_TRUE(cap[0].present);
}

TEST(registry_core, List_IsSortedByNameThenVersion) {
    reg_entry_t list[4];

    TEST_ASSERT_EQUAL_INT(0, installPlain("b:1", "x"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("a:2", "x"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("a:1", "x"));
    TEST_ASSERT_EQUAL_INT(3, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_STRING("a", list[0].name);
    TEST_ASSERT_EQUAL_STRING("1", list[0].version);
    TEST_ASSERT_EQUAL_STRING("a", list[1].name);
    TEST_ASSERT_EQUAL_STRING("2", list[1].version);
    TEST_ASSERT_EQUAL_STRING("b", list[2].name);
}

TEST(registry_core, List_ReportsTheTotalWhenTheBufferIsShort) {
    reg_entry_t list[1];

    TEST_ASSERT_EQUAL_INT(0, installPlain("a:1", "x"));
    TEST_ASSERT_EQUAL_INT(0, installPlain("b:1", "x"));
    TEST_ASSERT_EQUAL_INT(2, RegistryRead(list, 1));
    TEST_ASSERT_EQUAL_STRING("a", list[0].name);
}

TEST(registry_core, RefLongerThanTheLimit_IsRefused) {
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG,
                          RegistryWrite(START_WRITE, "averyveryverylongname:1",
                                        (const uint8_t *)"x", 1));
}

TEST_GROUP_RUNNER(registry_core) {
    RUN_TEST_CASE(registry_core, PlainInstall_ListsOneLayerEntry);
    RUN_TEST_CASE(registry_core, PlainInstall_Replaces_FreesTheOldBlob);
    RUN_TEST_CASE(registry_core, StagedLayers_AreHiddenUntilCommit);
    RUN_TEST_CASE(registry_core, Layers_ArriveInAnyOrder_CommitKeepsIndexOrder);
    RUN_TEST_CASE(registry_core, Commit_MissingLayer_FailsAndShowsNothing);
    RUN_TEST_CASE(registry_core, Commit_CountOutOfRange_IsRefused);
    RUN_TEST_CASE(registry_core, LayerIndex_AtTheLimit_IsRefused);
    RUN_TEST_CASE(registry_core, Commit_RecordsTheBuildTime);
    RUN_TEST_CASE(registry_core,
                  Commit_StampsTheInstallTimeWhenTheClockIsValid);
    RUN_TEST_CASE(registry_core, Commit_WithAnUnsetClock_FlagsTheRecord);
    RUN_TEST_CASE(registry_core, Resend_ReplacesTheStagedLayer);
    RUN_TEST_CASE(registry_core, Commit_DropsLayersAboveTheCount);
    RUN_TEST_CASE(registry_core,
                  SharedLayer_IsStoredOnce_AndSurvivesOneRemoval);
    RUN_TEST_CASE(registry_core, Remove_FreesEveryBlobOfTheEntry);
    RUN_TEST_CASE(registry_core,
                  MappedBlob_SurvivesRemoval_UntilTheWappUnloads);
    RUN_TEST_CASE(registry_core, ReplaceWhileRunning_KeepsTheOldBlobMapped);
    RUN_TEST_CASE(registry_core, Load_MapsLayersTopmostFirst);
    RUN_TEST_CASE(registry_core,
                  BareName_TakesTheNewestInstall_NotTheHighestVersion);
    RUN_TEST_CASE(registry_core, BareName_ServesTheRecordOfTheEntryItResolves);
    RUN_TEST_CASE(registry_core, InstallSequence_ContinuesAfterARestart);
    RUN_TEST_CASE(registry_core, SeededEntry_CannotBeRemoved);
    RUN_TEST_CASE(registry_core, InstallOverASeed_ClearsTheSeededFlag);
    RUN_TEST_CASE(registry_core, SecondInstall_WhileOneIsOpen_ReturnsEbusy);
    RUN_TEST_CASE(registry_core, StreamArrivesInChunks_AndIsHashedWhole);
    RUN_TEST_CASE(registry_core,
                  StoreWithNoRoom_FailsTheStageAndLeavesNoneOpen);
    RUN_TEST_CASE(registry_core, CutStream_FailingMidway_CommitsNothing);
    RUN_TEST_CASE(registry_core, Restart_ReclaimsBlobsNoEntryLists);
    RUN_TEST_CASE(registry_core, Signature_OnAStagedRef_ReturnsEnoent);
    RUN_TEST_CASE(registry_core,
                  Commit_StartsUnsigned_AndASignatureLandsAfterIt);
    RUN_TEST_CASE(registry_core, Capacity_ReportsEachStore);
    RUN_TEST_CASE(registry_core, List_IsSortedByNameThenVersion);
    RUN_TEST_CASE(registry_core, List_ReportsTheTotalWhenTheBufferIsShort);
    RUN_TEST_CASE(registry_core, RefLongerThanTheLimit_IsRefused);
}
