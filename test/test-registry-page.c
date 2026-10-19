/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <registry-files.h>
#include <registry-page.h>
#include <registry.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* registry_page — the page store on a simulated flash that enforces the erase
 * rule and loses power at a chosen operation. */

#define SIM_UNIT 4096
#define SIM_UNITS 16
#define SIM_SIZE (SIM_UNIT * SIM_UNITS)

typedef struct {
    uint8_t mem[SIM_SIZE];
    long ops;
    long cutAt; /* the operation that loses power; -1 for never */
    bool dead;
    int violations; /* writes that targeted bytes that were not erased */
    int maps;
} sim_t;

static sim_t sim;

/* The operation that is cut takes half its effect, as a real one would. */
static bool tick(void) {
    if (sim.dead)
        return false;
    sim.ops++;
    return !(sim.cutAt >= 0 && sim.ops >= sim.cutAt);
}

static int simErase(void *ctx, uint32_t unit) {
    bool whole = tick();
    size_t n = whole ? SIM_UNIT : SIM_UNIT / 2;

    (void)ctx;
    if (sim.dead)
        return -EIO;
    memset(sim.mem + ((size_t)unit * SIM_UNIT), 0xff, n);
    if (!whole) {
        sim.dead = true;
        return -EIO;
    }
    return 0;
}

static int simWrite(void *ctx, uint32_t offset, const uint8_t *buf, size_t n) {
    bool whole = tick();
    size_t take = whole ? n : n / 2;

    (void)ctx;
    if (sim.dead)
        return -EIO;
    for (size_t i = 0; i < take; i++) {
        if (sim.mem[offset + i] != 0xff)
            sim.violations++;
        sim.mem[offset + i] &= buf[i];
    }
    if (!whole) {
        sim.dead = true;
        return -EIO;
    }
    return 0;
}

static int simMap(void *ctx, const registry_extent_t *ext, size_t nExt,
                  size_t len, const uint8_t **ptr, void **handle) {
    uint8_t *joined;
    size_t at = 0;

    (void)ctx;
    sim.maps++;
    if (nExt == 1) {
        *ptr = sim.mem + ext[0].offset;
        *handle = NULL;
        return 0;
    }
    joined = WantedMalloc(len);
    if (joined == NULL)
        return -ENOMEM;
    for (size_t i = 0; i < nExt; i++) {
        memcpy(joined + at, sim.mem + ext[i].offset, ext[i].length);
        at += ext[i].length;
    }
    *ptr = joined;
    *handle = joined;
    return 0;
}

static void simUnmap(void *ctx, void *handle) {
    (void)ctx;
    sim.maps--;
    WantedFree(handle);
}

static int simBarrier(void *ctx) {
    (void)ctx;
    return sim.dead ? -EIO : 0;
}

static const registry_flash_ops_t simOps = {
    .erase = simErase,
    .write = simWrite,
    .map = simMap,
    .unmap = simUnmap,
    .barrier = simBarrier,
};

TEST_GROUP(registry_page);

static char origin[PATH_MAX];
static char scratch[PATH_MAX];
static char entryDir[PATH_MAX];
static char blobDir[PATH_MAX];
static registry_page_config_t cfg;
static registry_store_t *store;
static wapp_t wapp;

static void removeTree(const char *path) {
    DIR *d = opendir(path);
    const struct dirent *de;
    char child[PATH_MAX];

    if (d == NULL) {
        remove(path);
        return;
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        removeTree(child);
    }
    closedir(d);
    rmdir(path);
}

/* A fresh partition and directories, then the store started on them. */
static void boot(bool scattered) {
    char tmpl[] = "/tmp/registry-page-XXXXXX";

    TEST_ASSERT_NOT_NULL(mkdtemp(tmpl));
    strcpy(scratch, tmpl);
    snprintf(entryDir, sizeof(entryDir), "%s/entries", scratch);
    snprintf(blobDir, sizeof(blobDir), "%s/blobs", scratch);
    memset(&sim, 0, sizeof(sim));
    memset(sim.mem, 0xff, sizeof(sim.mem));
    sim.cutAt = -1;
    cfg = (registry_page_config_t){
        .name = "pages",
        .entryDir = entryDir,
        .blobDir = blobDir,
        .unit = SIM_UNIT,
        .units = SIM_UNITS,
        .scattered = scattered,
        .flash = &simOps,
    };
    store = RegistryPageStore(&cfg);
    DummyRegistryUseStore(store);
}

/* A reset: the partition and the records stay, everything in RAM starts over.
 */
static void reboot(void) {
    RegistryDeinit();
    sim.dead = false;
    sim.cutAt = -1;
    store = RegistryPageStore(&cfg);
    DummyRegistryUseStore(store);
}

TEST_SETUP(registry_page) {
    TEST_ASSERT_NOT_NULL(getcwd(origin, sizeof(origin)));
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
    memset(&wapp, 0, sizeof(wapp));
    scratch[0] = '\0';
}

TEST_TEAR_DOWN(registry_page) {
    DummyRegistryUseStore(NULL);
    if (scratch[0] != '\0')
        removeTree(scratch);
}

/* Bytes that identify their own position, so a mapping that joins units in the
 * wrong order shows. */
static void fill(uint8_t *out, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++)
        out[i] = (uint8_t)(seed + (i * 7) + (i >> 8));
}

static int stageBytes(const char *ref, unsigned layer, const uint8_t *data,
                      size_t n) {
    size_t off = 0;
    int rc;

    /* The stream arrives in uneven chunks, as it does over a link. */
    size_t chunk = n < 1000 ? n : 1000;

    rc = RegistryWriteLayer(START_WRITE, ref, layer, data, chunk);
    if (rc < 0)
        return rc;
    off = chunk;
    while (off < n) {
        chunk = n - off < 1500 ? n - off : 1500;
        rc = RegistryWriteLayer(CONTINUE_WRITE, NULL, 0, data + off, chunk);
        if (rc < 0) {
            RegistryWriteLayer(FINISH_WRITE, NULL, 0, NULL, 0);
            return rc;
        }
        off += chunk;
    }
    return RegistryWriteLayer(FINISH_WRITE, NULL, 0, NULL, 0);
}

static int installImage(const char *ref, size_t nLayers, const size_t *sizes,
                        uint8_t seed) {
    uint8_t *buf = WantedMalloc(SIM_SIZE + (2 * SIM_UNIT));
    int rc = 0;

    TEST_ASSERT_NOT_NULL(buf);
    for (size_t i = 0; i < nLayers && rc == 0; i++) {
        fill(buf, sizes[i], (uint8_t)(seed + (i * 31)));
        rc = stageBytes(ref, (unsigned)i, buf, sizes[i]);
    }
    WantedFree(buf);
    if (rc < 0)
        return rc;
    return RegistryCommit(ref, (unsigned)nLayers, false, 0);
}

static void checkInvariants(void) {
    registry_page_report_t rep;

    TEST_ASSERT_EQUAL_INT(0, RegistryPageCheck(store, &rep));
    TEST_ASSERT_EQUAL_UINT32(0, rep.shared);
    TEST_ASSERT_EQUAL_UINT32(0, rep.outOfRange);
    TEST_ASSERT_EQUAL_UINT32(0, rep.mismatch);
    TEST_ASSERT_EQUAL_INT(0, sim.violations);
}

static uint32_t freeUnits(void) {
    registry_capacity_t cap;

    TEST_ASSERT_EQUAL_INT(1, RegistryCapacity(&cap, 1));
    return cap.freeBytes / SIM_UNIT;
}

/* Maps every layer of the entry and compares it with what the record says. */
static void verifyEntry(const char *name, const char *version, size_t nLayers,
                        const size_t *sizes, uint8_t seed) {
    reg_entry_t e = MakeEntry(name, version, 0);
    uint8_t *expect = WantedMalloc(SIM_SIZE);

    TEST_ASSERT_NOT_NULL(expect);
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));
    TEST_ASSERT_EQUAL_UINT8(nLayers, wapp.layer_cnt);
    for (size_t i = 0; i < nLayers; i++) {
        size_t top = nLayers - 1 - i; /* layers[0] is the topmost */

        fill(expect, sizes[top], (uint8_t)(seed + (top * 31)));
        TEST_ASSERT_EQUAL_UINT32(sizes[top], wapp.layer_lens[i]);
        TEST_ASSERT_EQUAL_MEMORY(expect, wapp.layers[i], sizes[top]);
    }
    RegistryUnload(&wapp);
    memset(&wapp, 0, sizeof(wapp));
    WantedFree(expect);
}

TEST(registry_page, SmallBlob_TakesOneUnit) {
    static const size_t sizes[] = {100};

    boot(true);
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS, freeUnits());
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 1, sizes, 1));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS - 1, freeUnits());
    verifyEntry("app", "1", 1, sizes, 1);
    checkInvariants();
}

TEST(registry_page, LargeBlob_SpansUnitsAndReadsBackWhole) {
    static const size_t sizes[] = {(3 * SIM_UNIT) + 123};

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, installImage("big:1", 1, sizes, 9));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS - 4, freeUnits());
    verifyEntry("big", "1", 1, sizes, 9);
    checkInvariants();
}

TEST(registry_page, MultiLayerImage_KeepsEachLayerSeparate) {
    static const size_t sizes[] = {5000, 20, 9000};

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 3, sizes, 4));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS - 6, freeUnits());
    verifyEntry("app", "1", 3, sizes, 4);
    checkInvariants();
}

TEST(registry_page, Remove_FreesTheUnitsWithoutErasing) {
    static const size_t sizes[] = {6000};
    reg_entry_t e = MakeEntry("app", "1", 0);
    long ops;

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 1, sizes, 1));
    ops = sim.ops;
    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS, freeUnits());
    TEST_ASSERT_EQUAL_INT(ops, sim.ops);
    checkInvariants();
}

TEST(registry_page, SharedLayer_HoldsItsUnitsOnce) {
    static const size_t sizes[] = {5000};

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, installImage("one:1", 1, sizes, 3));
    TEST_ASSERT_EQUAL_INT(0, installImage("two:1", 1, sizes, 3));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS - 2, freeUnits());
    checkInvariants();
}

TEST(registry_page, Restart_RebuildsTheAllocationFromTheRecords) {
    static const size_t a[] = {5000, 100};
    static const size_t b[] = {9000};
    uint32_t before;

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, installImage("a:1", 2, a, 1));
    TEST_ASSERT_EQUAL_INT(0, installImage("b:1", 1, b, 2));
    before = freeUnits();

    reboot();
    TEST_ASSERT_EQUAL_UINT32(before, freeUnits());
    verifyEntry("a", "1", 2, a, 1);
    verifyEntry("b", "1", 1, b, 2);
    checkInvariants();
}

TEST(registry_page, ScatteredStore_UsesFragmentedFreeUnits) {
    static const size_t one[] = {SIM_UNIT};
    static const size_t two[] = {2 * SIM_UNIT};
    char ref[16];

    boot(true);
    for (int i = 0; i < SIM_UNITS; i++) {
        snprintf(ref, sizeof(ref), "f%d:1", i);
        TEST_ASSERT_EQUAL_INT(0, installImage(ref, 1, one, (uint8_t)i));
    }
    for (int i = 0; i < SIM_UNITS; i += 2) {
        reg_entry_t e;

        snprintf(ref, sizeof(ref), "f%d", i);
        e = MakeEntry(ref, "1", 0);
        TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    }
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS / 2, freeUnits());
    TEST_ASSERT_EQUAL_INT(0, installImage("wide:1", 1, two, 5));
    verifyEntry("wide", "1", 1, two, 5);
    checkInvariants();
}

TEST(registry_page, ContiguousStore_RefusesWhatFragmentationLeavesNoRoomFor) {
    static const size_t one[] = {SIM_UNIT};
    static const size_t two[] = {2 * SIM_UNIT};
    registry_capacity_t cap;
    char ref[16];

    boot(false);
    for (int i = 0; i < SIM_UNITS; i++) {
        snprintf(ref, sizeof(ref), "f%d:1", i);
        TEST_ASSERT_EQUAL_INT(0, installImage(ref, 1, one, (uint8_t)i));
    }
    for (int i = 0; i < SIM_UNITS; i += 2) {
        reg_entry_t e;

        snprintf(ref, sizeof(ref), "f%d", i);
        e = MakeEntry(ref, "1", 0);
        TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    }
    TEST_ASSERT_EQUAL_INT(1, RegistryCapacity(&cap, 1));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS / 2 * SIM_UNIT, cap.freeBytes);
    TEST_ASSERT_EQUAL_UINT32(SIM_UNIT, cap.largestFree);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, installImage("wide:1", 1, two, 5));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0) - SIM_UNITS / 2);
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS / 2, freeUnits());
    TEST_ASSERT_EQUAL_INT(0, installImage("narrow:1", 1, one, 6));
    checkInvariants();
}

TEST(registry_page, ContiguousStore_MapsEachBlobAsOneExtent) {
    static const size_t sizes[] = {(2 * SIM_UNIT) + 50};

    boot(false);
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 1, sizes, 7));
    verifyEntry("app", "1", 1, sizes, 7);
    TEST_ASSERT_EQUAL_INT(0, sim.maps);
}

TEST(registry_page, FullPartition_ReturnsEnospcAndFreesTheStage) {
    static const size_t sizes[] = {(SIM_UNITS + 1) * SIM_UNIT};
    static const size_t small[] = {10};

    boot(true);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, installImage("huge:1", 1, sizes, 1));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS, freeUnits());
    TEST_ASSERT_EQUAL_INT(0, installImage("small:1", 1, small, 2));
    checkInvariants();
}

TEST(registry_page, Capacity_ReportsUnitsAndKind) {
    registry_capacity_t cap;

    boot(true);
    TEST_ASSERT_EQUAL_INT(1, RegistryCapacity(&cap, 1));
    TEST_ASSERT_EQUAL_STRING("pages", cap.name);
    TEST_ASSERT_EQUAL_STRING("page", cap.kind);
    TEST_ASSERT_EQUAL_UINT32(SIM_UNIT, cap.unit);
    TEST_ASSERT_EQUAL_UINT32(SIM_SIZE, cap.total);
    TEST_ASSERT_EQUAL_UINT32(SIM_SIZE, cap.largestFree);
    TEST_ASSERT_TRUE((cap.caps & REGISTRY_CAP_SCATTERED) != 0);
}

/* A hand-made blob record: magic, one extent, a length, the digest. */
static void plantBlobRecord(const char *hex, uint32_t offset, uint32_t length) {
    uint8_t rec[44 + 8];
    uint8_t digest[REGISTRY_DIGEST_LEN];

    memset(rec, 0, sizeof(rec));
    rec[0] = 'W';
    rec[1] = 'B';
    rec[2] = 'L';
    rec[3] = '1';
    rec[4] = 1;
    rec[8] = (uint8_t)length;
    rec[9] = (uint8_t)(length >> 8);
    for (size_t i = 0; i < REGISTRY_DIGEST_LEN; i++) {
        char pair[3] = {hex[2 * i], hex[(2 * i) + 1], '\0'};

        digest[i] = (uint8_t)strtoul(pair, NULL, 16);
    }
    memcpy(rec + 12, digest, sizeof(digest));
    rec[44] = (uint8_t)offset;
    rec[45] = (uint8_t)(offset >> 8);
    rec[46] = (uint8_t)(offset >> 16);
    rec[48] = (uint8_t)length;
    rec[49] = (uint8_t)(length >> 8);
    TEST_ASSERT_EQUAL_INT(0, RegFilesPut(blobDir, hex, rec, sizeof(rec)));
}

TEST(registry_page, Restart_DiscardsABlobRecordPastThePartition) {
    static const char hex[] =
        "aa00000000000000000000000000000000000000000000000000000000000001";
    uint8_t digests[4][REGISTRY_DIGEST_LEN];
    registry_capacity_t cap;

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, mkdir(blobDir, 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir(entryDir, 0755));
    plantBlobRecord(hex, SIM_SIZE - 100, 5000);
    reboot();
    TEST_ASSERT_EQUAL_INT(0, store->ops->init(store));
    TEST_ASSERT_EQUAL_INT(0, store->ops->blobList(store, digests, 4));
    TEST_ASSERT_EQUAL_INT(0, store->ops->capacity(store, &cap));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS, cap.freeBytes / SIM_UNIT);
}

TEST(registry_page, Restart_DiscardsABlobRecordThatOverlapsAnother) {
    static const char hexA[] =
        "bb00000000000000000000000000000000000000000000000000000000000001";
    static const char hexB[] =
        "bb00000000000000000000000000000000000000000000000000000000000002";
    uint8_t digests[4][REGISTRY_DIGEST_LEN];
    registry_capacity_t cap;

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, mkdir(blobDir, 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir(entryDir, 0755));
    plantBlobRecord(hexA, SIM_UNIT, 4000);
    plantBlobRecord(hexB, SIM_UNIT + 10, 100);
    reboot();
    TEST_ASSERT_EQUAL_INT(0, store->ops->init(store));
    TEST_ASSERT_EQUAL_INT(1, store->ops->blobList(store, digests, 4));
    TEST_ASSERT_EQUAL_INT(0, store->ops->capacity(store, &cap));
    TEST_ASSERT_EQUAL_UINT32(SIM_UNITS - 1, cap.freeBytes / SIM_UNIT);
    checkInvariants();
}

TEST(registry_page, Restart_RemovesAStrayStageFile) {
    uint8_t one = 1;

    boot(true);
    TEST_ASSERT_EQUAL_INT(0, mkdir(blobDir, 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir(entryDir, 0755));
    TEST_ASSERT_EQUAL_INT(0, RegFilesPut(blobDir, "x", &one, 1));
    rename("x", "x");
    {
        char stray[PATH_MAX];
        FILE *f;

        snprintf(stray, sizeof(stray), "%s/_stage", blobDir);
        f = fopen(stray, "wb");
        TEST_ASSERT_NOT_NULL(f);
        fclose(f);
        reboot();
        RegistryRead(NULL, 0);
        TEST_ASSERT_NULL(fopen(stray, "rb"));
    }
}

/* One image of two layers replaces another, and power is lost at operation
 * `cut`. After the reset the entry is whole: the old image or the new one,
 * never a mix, and nothing leaks. */
static int cutRun(bool scattered, long cut, long *opsOut) {
    static const size_t oldSizes[] = {6000, 3000};
    static const size_t newSizes[] = {2500, 9000};
    registry_meta_t meta;
    reg_entry_t e = MakeEntry("app", "1", 0);
    reg_entry_t list[4];
    bool wasNew;
    int n;

    boot(scattered);
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 2, oldSizes, 1));
    sim.ops = 0;
    sim.cutAt = cut;
    installImage("app:1", 2, newSizes, 50);
    if (opsOut != NULL)
        *opsOut = sim.ops;

    reboot();
    n = RegistryRead(list, 4);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    wasNew = meta.layerLen[0] == newSizes[0];
    if (wasNew)
        verifyEntry("app", "1", 2, newSizes, 50);
    else
        verifyEntry("app", "1", 2, oldSizes, 1);
    checkInvariants();

    /* The units a cut left half-programmed are erased before they are used. */
    {
        static const size_t more[] = {3 * SIM_UNIT};

        TEST_ASSERT_EQUAL_INT(0, installImage("more:1", 1, more, 99));
        verifyEntry("more", "1", 1, more, 99);
        checkInvariants();
    }
    DummyRegistryUseStore(NULL);
    removeTree(scratch);
    scratch[0] = '\0';
    return wasNew ? 1 : 0;
}

static void sweep(bool scattered) {
    long total = 0;
    int sawOld = 0;
    int sawNew = 0;

    cutRun(scattered, -1, &total);
    TEST_ASSERT_TRUE(total > 10);
    /* Every operation up to the commit, and the commit itself. */
    for (long cut = 1; cut <= total + 2; cut++) {
        if (cutRun(scattered, cut, NULL))
            sawNew++;
        else
            sawOld++;
    }
    TEST_ASSERT_TRUE(sawOld > 0);
    TEST_ASSERT_TRUE(sawNew > 0);
}

TEST(registry_page, PowerCut_AtEveryOperation_LeavesAWholeEntry_Scattered) {
    sweep(true);
}

TEST(registry_page, PowerCut_AtEveryOperation_LeavesAWholeEntry_Contiguous) {
    sweep(false);
}

TEST(registry_page, PowerCut_DuringAFirstInstall_LeavesNothingOrTheWholeImage) {
    static const size_t sizes[] = {7000, 100};
    registry_meta_t meta;
    reg_entry_t e = MakeEntry("app", "1", 0);
    long total = 0;
    int whole = 0;
    int none = 0;

    boot(true);
    sim.cutAt = -1;
    TEST_ASSERT_EQUAL_INT(0, installImage("app:1", 2, sizes, 1));
    total = sim.ops;
    DummyRegistryUseStore(NULL);
    removeTree(scratch);

    for (long cut = 1; cut <= total + 1; cut++) {
        boot(true);
        sim.cutAt = cut;
        installImage("app:1", 2, sizes, 1);
        reboot();
        if (RegistryRead(NULL, 0) == 0) {
            none++;
            TEST_ASSERT_EQUAL_UINT32(SIM_UNITS, freeUnits());
        } else {
            whole++;
            TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
            verifyEntry("app", "1", 2, sizes, 1);
        }
        checkInvariants();
        DummyRegistryUseStore(NULL);
        removeTree(scratch);
    }
    scratch[0] = '\0';
    TEST_ASSERT_TRUE(none > 0);
    TEST_ASSERT_TRUE(whole > 0);
}

TEST_GROUP_RUNNER(registry_page) {
    RUN_TEST_CASE(registry_page, SmallBlob_TakesOneUnit);
    RUN_TEST_CASE(registry_page, LargeBlob_SpansUnitsAndReadsBackWhole);
    RUN_TEST_CASE(registry_page, MultiLayerImage_KeepsEachLayerSeparate);
    RUN_TEST_CASE(registry_page, Remove_FreesTheUnitsWithoutErasing);
    RUN_TEST_CASE(registry_page, SharedLayer_HoldsItsUnitsOnce);
    RUN_TEST_CASE(registry_page, Restart_RebuildsTheAllocationFromTheRecords);
    RUN_TEST_CASE(registry_page, ScatteredStore_UsesFragmentedFreeUnits);
    RUN_TEST_CASE(registry_page,
                  ContiguousStore_RefusesWhatFragmentationLeavesNoRoomFor);
    RUN_TEST_CASE(registry_page, ContiguousStore_MapsEachBlobAsOneExtent);
    RUN_TEST_CASE(registry_page, FullPartition_ReturnsEnospcAndFreesTheStage);
    RUN_TEST_CASE(registry_page, Capacity_ReportsUnitsAndKind);
    RUN_TEST_CASE(registry_page, Restart_DiscardsABlobRecordPastThePartition);
    RUN_TEST_CASE(registry_page,
                  Restart_DiscardsABlobRecordThatOverlapsAnother);
    RUN_TEST_CASE(registry_page, Restart_RemovesAStrayStageFile);
    RUN_TEST_CASE(registry_page,
                  PowerCut_AtEveryOperation_LeavesAWholeEntry_Scattered);
    RUN_TEST_CASE(registry_page,
                  PowerCut_AtEveryOperation_LeavesAWholeEntry_Contiguous);
    RUN_TEST_CASE(registry_page,
                  PowerCut_DuringAFirstInstall_LeavesNothingOrTheWholeImage);
}
