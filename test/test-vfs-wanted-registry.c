/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <platform.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include <tiny-json.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_registry_driver — the "reg" virtual driver (vfs-wanted-registry.c) driven
 * directly through its vtable, backed by the dummy in-memory registry.
 * ═══════════════════════════════════════════════════════════════════════════ */

extern const vfs_driver_t WantedRegistryDriver;

TEST_GROUP(vfs_registry_driver);

static const vfs_driver_t *drv;

static void SeedTwo(void) {
    reg_entry_t seed[2] = {
        MakeEntry("app1", "1.0.0", 42),
        MakeEntry("app2", "2.3.4", 84),
    };
    DummyRegistrySeed(seed, 2);
}

TEST_SETUP(vfs_registry_driver) {
    DummyRegistryReset();
    drv = &WantedRegistryDriver;
}

TEST_TEAR_DOWN(vfs_registry_driver) {
    /* Leave the driver closed so the function-static EOF flag in _Read resets. */
    drv->Close(drv->ctx, 0);
}

TEST(vfs_registry_driver, OpenRoot_LoadsEntries) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDONLY));

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, 0, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(2, st.size); /* size of dir fd == entry count */
}

TEST(vfs_registry_driver, OpenNullPath_ReturnsEinval) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, NULL, VFS_O_RDONLY));
}

TEST(vfs_registry_driver, OpenEntryByName_ReturnsOneBasedFd) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(1, drv->Open(drv->ctx, "app1", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(2, drv->Open(drv->ctx, "app2", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, OpenEntryByNameVersion_ReturnsFd) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(2, drv->Open(drv->ctx, "app2:2.3.4", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, OpenUnknown_ReturnsEnoent) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, "ghost", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, StatEntry_IsRegularFile) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, 1, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(42, st.size);
}

TEST(vfs_registry_driver, Stat_BadFd_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Stat(drv->ctx, 99, &st));
}

TEST(vfs_registry_driver, ReadRoot_IsDirectory) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    /* The registry root is a directory; reading it as a file is rejected.
     * Enumeration is via ReadDir. */
    uint8_t buf[256] = {0};
    TEST_ASSERT_EQUAL_INT(-EISDIR, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_registry_driver, ReadEntry_DescriptorSynthesized) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    /* fd>0 reads a small JSON descriptor synthesized from the registry entry
     * (name/version/size) plus the image's declared linear-memory profile,
     * parsed from the image header (the dummy serves a canned (memory 1 4)). */
    uint8_t buf[160] = {0};
    int n = drv->Read(drv->ctx, 1, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "app1", 4));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "1.0.0", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "42", 2));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "\"init_pages\":1", 14));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "\"max_pages\":4", 13));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "\"can_grow\":true", 15));
}

TEST(vfs_registry_driver, Read_NullBuf_ReturnsEinval) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Read(drv->ctx, 0, NULL, 16));
}

TEST(vfs_registry_driver, Read_NotOpened_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    drv->Close(drv->ctx, 0);

    uint8_t buf[16];
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_registry_driver, ReadDir_ListsNameVersionPairs) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0,
        drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "app1", 4));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "2.3.4", 5));
}

TEST(vfs_registry_driver, Write_EntryFd_ReturnsErofs) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Write(drv->ctx, 1, "x", 1));
}

TEST(vfs_registry_driver, Write_NotOpened_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    drv->Close(drv->ctx, 0);
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Write(drv->ctx, 0, "x", 1));
}

TEST(vfs_registry_driver, OpenForWrite_ByRef_ReturnsWriteFd) {
    /* Opening a "<name>:<ver>" path for write is an install: it names the image
     * by the ref and returns the root write fd (0). */
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "newapp:1.0.0-1",
                                       VFS_O_WRONLY));
}

TEST(vfs_registry_driver, WriteRootNoRef_ReturnsErofs) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    /* The registry root is not writable; an install must open by ref. */
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Write(drv->ctx, 0, "{}", 2));
}

TEST(vfs_registry_driver, WriteByRef_Chunk_Unsupported) {
    drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY);
    /* WantedWriteRegistry -> PlatformRegistryWrite(START_WRITE) -> -ENOSYS. */
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, 0, "{}", 2));
}

/* An out-of-grammar install ref is rejected at open rather than naming a file.
 * The grammar is "<name>[:<tag>]", each component [A-Za-z0-9_][A-Za-z0-9._-]*
 * and within its length bound. */
TEST(vfs_registry_driver, OpenForWrite_InvalidRef_ReturnsEinval) {
    /* whitespace is not in the grammar (name and tag halves) */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "bad name:1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:bad tag", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app name", VFS_O_WRONLY));
    /* a tag carries no second separator */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:1:2", VFS_O_WRONLY));
    /* empty name or empty tag */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, ":1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:", VFS_O_WRONLY));
    /* component may not start with a separator-class char */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "-app:1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:.1.0", VFS_O_WRONLY));
    /* a stray punctuation char outside the class */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:tag!", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "ap/p:1.0", VFS_O_WRONLY));
    /* name at/over its length bound (16 ≥ WAPP_MAX_NAME_LEN) */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "aaaaaaaaaaaaaaaa:1", VFS_O_WRONLY));
    /* tag at/over its length bound (16 ≥ WAPP_MAX_VERSION_LEN) */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:aaaaaaaaaaaaaaaa", VFS_O_WRONLY));
}

/* In-grammar refs are accepted — bare name (first-match) and pinned tags,
 * numeric or not — returning the root write fd (0). */
TEST(vfs_registry_driver, OpenForWrite_ValidRefs_Accepted) {
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "app", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "app:1.0.0-1", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "app:stable", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "my_app.v2:latest", VFS_O_WRONLY));
}

TEST(vfs_registry_driver, Read_FdBeyondEntries_ReturnsEinval) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Read(drv->ctx, 99, buf, sizeof(buf)));
}

TEST(vfs_registry_driver, Write_NullBuf_ReturnsEinval) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Write(drv->ctx, 0, NULL, 1));
}

TEST(vfs_registry_driver, ReadDir_NullBuf_ReturnsEinval) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL,
        drv->ReadDir(drv->ctx, 0, NULL, 0, &cookie, &used));
}

TEST(vfs_registry_driver, ReadDir_NotOpened_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    drv->Close(drv->ctx, 0);
    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF,
        drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
}

TEST(vfs_registry_driver, Unlink_ByNameVersion_RemovesEntry) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(0, drv->Unlink(drv->ctx, 0, "app2:2.3.4"));
    TEST_ASSERT_EQUAL_INT(1, PlatformRegistryRead(NULL, 0));
}

TEST(vfs_registry_driver, Unlink_RemovesEntry) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    TEST_ASSERT_EQUAL_INT(0, drv->Unlink(drv->ctx, 0, "app1"));
    TEST_ASSERT_EQUAL_INT(1, PlatformRegistryRead(NULL, 0));
}

TEST(vfs_registry_driver, Unlink_Unknown_ReturnsEnoent) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Unlink(drv->ctx, 0, "ghost"));
}

TEST(vfs_registry_driver, Close_AfterWrite_FinalizesRegistry) {
    drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY);
    drv->Write(drv->ctx, 0, "{}", 2); /* sets startedWriting */
    /* Close with a pending write calls WantedCloseRegistry ->
     * PlatformRegistryWrite(FINISH_WRITE) -> -ENOSYS. */
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Close(drv->ctx, 0));
}

TEST_GROUP_RUNNER(vfs_registry_driver) {
    RUN_TEST_CASE(vfs_registry_driver, OpenRoot_LoadsEntries);
    RUN_TEST_CASE(vfs_registry_driver, OpenNullPath_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntryByName_ReturnsOneBasedFd);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntryByNameVersion_ReturnsFd);
    RUN_TEST_CASE(vfs_registry_driver, OpenUnknown_ReturnsEnoent);
    RUN_TEST_CASE(vfs_registry_driver, StatEntry_IsRegularFile);
    RUN_TEST_CASE(vfs_registry_driver, Stat_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, ReadRoot_IsDirectory);
    RUN_TEST_CASE(vfs_registry_driver, ReadEntry_DescriptorSynthesized);
    RUN_TEST_CASE(vfs_registry_driver, Read_NullBuf_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, Read_NotOpened_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_ListsNameVersionPairs);
    RUN_TEST_CASE(vfs_registry_driver, Write_EntryFd_ReturnsErofs);
    RUN_TEST_CASE(vfs_registry_driver, Write_NotOpened_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, OpenForWrite_ByRef_ReturnsWriteFd);
    RUN_TEST_CASE(vfs_registry_driver, WriteRootNoRef_ReturnsErofs);
    RUN_TEST_CASE(vfs_registry_driver, WriteByRef_Chunk_Unsupported);
    RUN_TEST_CASE(vfs_registry_driver, OpenForWrite_InvalidRef_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, OpenForWrite_ValidRefs_Accepted);
    RUN_TEST_CASE(vfs_registry_driver, Read_FdBeyondEntries_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, Write_NullBuf_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_NullBuf_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_NotOpened_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_ByNameVersion_RemovesEntry);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_RemovesEntry);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_Unknown_ReturnsEnoent);
    RUN_TEST_CASE(vfs_registry_driver, Close_AfterWrite_FinalizesRegistry);
}
