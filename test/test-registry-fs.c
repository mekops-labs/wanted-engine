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

#include <platform-config.h>
#include <platform.h>
#include <registry-fs.h>
#include <registry.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* registry_fs — the filesystem store behind the registry core, on a real
 * directory. */

TEST_GROUP(registry_fs);

static char origin[PATH_MAX];
static char scratch[PATH_MAX];
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

TEST_SETUP(registry_fs) {
    char tmpl[] = "/tmp/registry-fs-XXXXXX";

    TEST_ASSERT_NOT_NULL(getcwd(origin, sizeof(origin)));
    TEST_ASSERT_NOT_NULL(mkdtemp(tmpl));
    strcpy(scratch, tmpl);
    TEST_ASSERT_EQUAL_INT(0, chdir(scratch));
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
    DummyRegistryUseStore(RegistryFsStore());
    memset(&wapp, 0, sizeof(wapp));
}

TEST_TEAR_DOWN(registry_fs) {
    DummyRegistryUseStore(NULL);
    TEST_ASSERT_EQUAL_INT(0, chdir(origin));
    removeTree(scratch);
}

static int install(const char *ref, const char *bytes) {
    int rc =
        RegistryWrite(START_WRITE, ref, (const uint8_t *)bytes, strlen(bytes));

    if (rc < 0)
        return rc;
    return RegistryWrite(FINISH_WRITE, NULL, NULL, 0);
}

static int stage(const char *ref, unsigned layer, const char *bytes) {
    int rc = RegistryWriteLayer(START_WRITE, ref, layer, (const uint8_t *)bytes,
                                strlen(bytes));

    if (rc < 0)
        return rc;
    return RegistryWriteLayer(FINISH_WRITE, NULL, 0, NULL, 0);
}

static int countFiles(const char *dir) {
    DIR *d = opendir(dir);
    const struct dirent *de;
    int n = 0;

    if (d == NULL)
        return -1;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] != '.')
            n++;
    }
    closedir(d);
    return n;
}

static bool fileExists(const char *path) {
    struct stat st;

    return stat(path, &st) == 0;
}

static void writeFile(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");

    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_UINT32(len, fwrite(data, 1, len, f));
    fclose(f);
}

TEST(registry_fs, InstallLoadRemove_UseFilesNamedByDigest) {
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);

    TEST_ASSERT_EQUAL_INT(0, install("app:1.0.0", "image-bytes"));
    TEST_ASSERT_EQUAL_INT(1, countFiles("registry/blobs"));
    TEST_ASSERT_EQUAL_INT(1, countFiles("registry/entries"));
    TEST_ASSERT_TRUE(fileExists("registry/entries/app@1.0.0.rec"));

    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));
    TEST_ASSERT_EQUAL_UINT32(11, wapp.layer_lens[0]);
    TEST_ASSERT_EQUAL_MEMORY("image-bytes", wapp.layers[0], 11);
    TEST_ASSERT_TRUE(RegistryUnload(&wapp));

    TEST_ASSERT_EQUAL_INT(0, RegistryRemove(&e));
    TEST_ASSERT_EQUAL_INT(0, countFiles("registry/blobs"));
    TEST_ASSERT_EQUAL_INT(0, countFiles("registry/entries"));
}

TEST(registry_fs, Entries_SurviveARestart) {
    reg_entry_t list[4];

    TEST_ASSERT_EQUAL_INT(0, stage("app:1.0.0", 0, "base"));
    TEST_ASSERT_EQUAL_INT(0, stage("app:1.0.0", 1, "top"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1.0.0", 2, true, 42));
    RegistryDeinit();

    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_STRING("app", list[0].name);
    TEST_ASSERT_EQUAL_UINT32(7, list[0].size);
    TEST_ASSERT_EQUAL_INT(2, countFiles("registry/blobs"));
}

TEST(registry_fs, SharedLayer_IsOneFile) {
    TEST_ASSERT_EQUAL_INT(0, stage("one:1", 0, "shared"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("one:1", 1, false, 0));
    TEST_ASSERT_EQUAL_INT(0, stage("two:1", 0, "shared"));
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("two:1", 1, false, 0));
    TEST_ASSERT_EQUAL_INT(1, countFiles("registry/blobs"));
    TEST_ASSERT_FALSE(fileExists("registry/_stage"));
}

TEST(registry_fs, Restart_ReclaimsAnUncommittedBlob_AndAStaleStage) {
    TEST_ASSERT_EQUAL_INT(0, stage("app:1.0.0", 0, "orphan"));
    writeFile("registry/_stage", "half", 4);
    TEST_ASSERT_EQUAL_INT(1, countFiles("registry/blobs"));
    RegistryDeinit();

    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, countFiles("registry/blobs"));
    TEST_ASSERT_FALSE(fileExists("registry/_stage"));
}

/* A "WAP3" record as the previous layout wrote it: magic, size, key id, layer
 * count, flags, two pad bytes, a digest for each layer slot, the signature. */
static size_t oldRecord(uint8_t *out, uint32_t size, uint8_t flags,
                        uint32_t keyId) {
    size_t n = 16 + REGISTRY_META_MAX_LAYERS * REGISTRY_META_DIGEST_LEN +
               REGISTRY_META_SIG_LEN;

    memset(out, 0, n);
    out[0] = '3';
    out[1] = 'P';
    out[2] = 'A';
    out[3] = 'W';
    out[4] = (uint8_t)size;
    out[8] = (uint8_t)keyId;
    out[12] = 1;
    out[13] = flags;
    memset(out + n - REGISTRY_META_SIG_LEN, 0x77, REGISTRY_META_SIG_LEN);
    return n;
}

TEST(registry_fs, OldLayout_IsConvertedInPlace_KeepingTheSignature) {
    uint8_t rec[REGISTRY_RECORD_MAX];
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);
    registry_meta_t meta;
    reg_entry_t list[4];

    TEST_ASSERT_EQUAL_INT(0, mkdir("registry", 0755));
    writeFile("registry/app@1.0.0.wapp", "old-image", 9);
    writeFile("registry/app@1.0.0.meta", rec,
              oldRecord(rec, 9, REGISTRY_META_SIGNED, 3));

    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_STRING("app", list[0].name);
    TEST_ASSERT_EQUAL_STRING("1.0.0", list[0].version);
    TEST_ASSERT_EQUAL_UINT32(9, list[0].size);

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT32(3, meta.keyId);
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_SIGNED) != 0);
    TEST_ASSERT_EQUAL_UINT8(0x77, meta.signature[0]);
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_CLOCK_UNSET) != 0);

    TEST_ASSERT_FALSE(fileExists("registry/app@1.0.0.wapp"));
    TEST_ASSERT_FALSE(fileExists("registry/app@1.0.0.meta"));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));
    TEST_ASSERT_EQUAL_MEMORY("old-image", wapp.layers[0], 9);
    RegistryUnload(&wapp);
}

TEST(registry_fs, OldLayout_WithNoRecord_ReadsAsUnsigned) {
    reg_entry_t e = MakeEntry("tool", "", 0);
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, mkdir("registry", 0755));
    writeFile("registry/tool.wapp", "tool-image", 10);

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT8(1, meta.layerCount);
    TEST_ASSERT_EQUAL_UINT32(10, meta.size);
    TEST_ASSERT_EQUAL_INT(0, meta.flags & REGISTRY_META_SIGNED);
}

TEST(registry_fs, OldLayout_ManyImages_AreAllConverted) {
    char name[PATH_MAX];
    reg_entry_t list[20];

    TEST_ASSERT_EQUAL_INT(0, mkdir("registry", 0755));
    for (int i = 0; i < 12; i++) {
        snprintf(name, sizeof(name), "registry/app%d@1.0.0.wapp", i);
        writeFile(name, name, strlen(name));
    }
    TEST_ASSERT_EQUAL_INT(12, RegistryRead(list, 20));
    TEST_ASSERT_EQUAL_INT(12, countFiles("registry/blobs"));
}

TEST(registry_fs, DroppedInImage_AppearsAndIsTheNewest) {
    reg_entry_t bare = MakeEntry("app", "", 0);
    reg_entry_t list[4];

    TEST_ASSERT_EQUAL_INT(0, install("app:1.0.0", "one"));
    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    writeFile("registry/app@0.9.0.wapp", "dropped", 7);

    TEST_ASSERT_EQUAL_INT(2, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&bare, &wapp));
    TEST_ASSERT_EQUAL_STRING("0.9.0", wapp.version);
    RegistryUnload(&wapp);
}

TEST(registry_fs, OldLayout_ARecordThatCannotLand_KeepsTheImageAndItsMeta) {
    uint8_t rec[REGISTRY_RECORD_MAX];
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);
    registry_meta_t meta;

    TEST_ASSERT_EQUAL_INT(0, mkdir("registry", 0755));
    TEST_ASSERT_EQUAL_INT(0, mkdir("registry/entries", 0755));
    /* A directory where the record goes makes its rename fail. */
    TEST_ASSERT_EQUAL_INT(0, mkdir("registry/entries/app@1.0.0.rec", 0755));
    writeFile("registry/app@1.0.0.wapp", "old-image", 9);
    writeFile("registry/app@1.0.0.meta", rec,
              oldRecord(rec, 9, REGISTRY_META_SIGNED, 3));

    RegistryRead(NULL, 0);
    TEST_ASSERT_TRUE(fileExists("registry/app@1.0.0.wapp"));
    TEST_ASSERT_TRUE(fileExists("registry/app@1.0.0.meta"));

    TEST_ASSERT_EQUAL_INT(0, rmdir("registry/entries/app@1.0.0.rec"));
    RegistryDeinit();
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT32(3, meta.keyId);
    TEST_ASSERT_TRUE((meta.flags & REGISTRY_META_SIGNED) != 0);
    TEST_ASSERT_FALSE(fileExists("registry/app@1.0.0.meta"));
}

TEST(registry_fs, OldLayout_ImagesConvertedTogether_ShareASequence) {
    reg_entry_t bare = MakeEntry("app", "", 0);
    reg_entry_t one = MakeEntry("app", "1.0.0", 0);
    reg_entry_t two = MakeEntry("app", "2.0.0", 0);
    registry_meta_t m1;
    registry_meta_t m2;

    TEST_ASSERT_EQUAL_INT(0, mkdir("registry", 0755));
    writeFile("registry/app@2.0.0.wapp", "two", 3);
    writeFile("registry/app@1.0.0.wapp", "one", 3);

    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&one, &m1));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&two, &m2));
    TEST_ASSERT_EQUAL_UINT32(m1.installSeq, m2.installSeq);
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&bare, &wapp));
    TEST_ASSERT_EQUAL_STRING("1.0.0", wapp.version);
    RegistryUnload(&wapp);
}

TEST(registry_fs, Capacity_ReportsTheFileStore) {
    registry_capacity_t cap[2];

    TEST_ASSERT_EQUAL_INT(1, RegistryCapacity(cap, 2));
    TEST_ASSERT_EQUAL_STRING("file", cap[0].kind);
    TEST_ASSERT_TRUE((cap[0].caps & REGISTRY_CAP_MAPPABLE) != 0);
}

TEST_GROUP_RUNNER(registry_fs) {
    RUN_TEST_CASE(registry_fs, InstallLoadRemove_UseFilesNamedByDigest);
    RUN_TEST_CASE(registry_fs, Entries_SurviveARestart);
    RUN_TEST_CASE(registry_fs, SharedLayer_IsOneFile);
    RUN_TEST_CASE(registry_fs,
                  Restart_ReclaimsAnUncommittedBlob_AndAStaleStage);
    RUN_TEST_CASE(registry_fs,
                  OldLayout_IsConvertedInPlace_KeepingTheSignature);
    RUN_TEST_CASE(registry_fs, OldLayout_WithNoRecord_ReadsAsUnsigned);
    RUN_TEST_CASE(registry_fs, OldLayout_ManyImages_AreAllConverted);
    RUN_TEST_CASE(registry_fs, DroppedInImage_AppearsAndIsTheNewest);
    RUN_TEST_CASE(registry_fs,
                  OldLayout_ARecordThatCannotLand_KeepsTheImageAndItsMeta);
    RUN_TEST_CASE(registry_fs,
                  OldLayout_ImagesConvertedTogether_ShareASequence);
    RUN_TEST_CASE(registry_fs, Capacity_ReportsTheFileStore);
}
