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

/* vfs_registry_driver — the "reg" virtual driver driven directly through its
 * vtable, backed by the dummy in-memory registry. */

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

/* Descriptors are numbered from the driver's own table, so a test that leaves
 * one open shifts the next test's numbering. Free them all between tests. */
static void CloseAll(void) {
    for (int i = 0; i < 16; i++)
        drv->Close(drv->ctx, i);
}

TEST_SETUP(vfs_registry_driver) {
    DummyRegistryReset();
    drv = &WantedRegistryDriver;
    CloseAll();
}

TEST_TEAR_DOWN(vfs_registry_driver) { CloseAll(); }

/* Open `path` for reading and return its descriptor, failing the test if the
 * open did not succeed. */
static int OpenEntry(const char *path) {
    int fd = drv->Open(drv->ctx, path, VFS_O_RDONLY);
    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
    return fd;
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

TEST(vfs_registry_driver, OpenEntry_ReturnsDistinctDescriptors) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(1, drv->Open(drv->ctx, "app1", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(2, drv->Open(drv->ctx, "app2", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, OpenEntryByNameVersion_ReturnsFd) {
    SeedTwo();
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "app2:2.3.4", VFS_O_RDONLY) >= 0);
}

TEST(vfs_registry_driver, OpenUnknown_ReturnsEnoent) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, "ghost", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, StatEntry_IsRegularFile) {
    SeedTwo();
    int fd = OpenEntry("app1");

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(42, st.size);
}

TEST(vfs_registry_driver, Stat_BadFd_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Stat(drv->ctx, 99, &st));
    /* never opened, but within the table */
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Stat(drv->ctx, 5, &st));
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

    /* fd>0 reads a small JSON descriptor synthesized from the registry entry
     * (name/version/size) plus the image's declared linear-memory profile,
     * parsed from the image header (the dummy serves a canned (memory 1 4)). */
    uint8_t buf[160] = {0};
    int n = drv->Read(drv->ctx, OpenEntry("app1"), buf, sizeof(buf));
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
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "app1", 4));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "2.3.4", 5));
}

TEST(vfs_registry_driver, Write_EntryFd_ReturnsErofs) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(-EROFS,
                          drv->Write(drv->ctx, OpenEntry("app1"), "x", 1));
}

TEST(vfs_registry_driver, Write_NotOpened_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    drv->Close(drv->ctx, 0);
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Write(drv->ctx, 0, "x", 1));
}

TEST(vfs_registry_driver, OpenForWrite_ByRef_ReturnsWriteFd) {
    /* Opening a "<name>:<ver>" path for write is an install: it names the image
     * by the ref and answers a descriptor the bytes go to. */
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY) >= 0);
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
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "bad name:1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:bad tag", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app name", VFS_O_WRONLY));
    /* a tag carries no second separator */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:1:2", VFS_O_WRONLY));
    /* empty name or empty tag */
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, ":1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:", VFS_O_WRONLY));
    /* component may not start with a separator-class char */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "-app:1.0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:.1.0", VFS_O_WRONLY));
    /* a stray punctuation char outside the class */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:tag!", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "ap/p:1.0", VFS_O_WRONLY));
    /* name at/over its length bound (16 ≥ WAPP_MAX_NAME_LEN) */
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, drv->Open(drv->ctx, "aaaaaaaaaaaaaaaa:1", VFS_O_WRONLY));
    /* tag at/over its length bound (16 ≥ WAPP_MAX_VERSION_LEN) */
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, drv->Open(drv->ctx, "app:aaaaaaaaaaaaaaaa", VFS_O_WRONLY));
}

/* In-grammar refs are accepted — bare name (first-match) and pinned tags,
 * numeric or not — each answering its own descriptor. */
TEST(vfs_registry_driver, OpenForWrite_ValidRefs_Accepted) {
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "app", VFS_O_WRONLY) >= 0);
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "app:1.0.0-1", VFS_O_WRONLY) >= 0);
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "app:stable", VFS_O_WRONLY) >= 0);
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "my_app.v2:latest", VFS_O_WRONLY) >=
                     0);
}

TEST(vfs_registry_driver, Read_FdBeyondEntries_ReturnsEbadf) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Read(drv->ctx, 99, buf, sizeof(buf)));
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
    TEST_ASSERT_EQUAL_INT(
        -EBADF, drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
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

TEST(vfs_registry_driver, Close_AfterFailedStart_DoesNotFinalize) {
    drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY);
    /* The dummy backing refuses START_WRITE, so nothing is in flight and the
     * close has nothing to finalize. */
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, 0, "{}", 2));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, 0));
}

/* A failed start must leave the write unarmed. Continuing after it reports the
 * backing's -EBADF for a write it never opened, hiding the error that actually
 * stopped the install — a full registry reads as a bad descriptor. */
TEST(vfs_registry_driver, WriteRegistry_FailedStart_LeavesWriteUnarmed) {
    bool cont = false;

    TEST_ASSERT_EQUAL_INT(
        -ENOSYS,
        WantedWriteRegistry(&cont, "newapp:1.0.0-1", (const uint8_t *)"{}", 2));
    TEST_ASSERT_FALSE(cont);
}

TEST(vfs_registry_driver, OpenEntry_WithoutOpeningRoot_Resolves) {
    /* A caller asking whether one image is installed never enumerates the
     * directory, so the lookup must load the entries itself. */
    SeedTwo();
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, "app1:1.0.0", VFS_O_RDONLY) >= 0);
}

TEST(vfs_registry_driver, OpenEntry_NamePrefix_ReturnsEnoent) {
    /* "app1" must not answer for a longer name that starts with it. */
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, "app1x", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "app1x:1.0.0", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, OpenEntry_WrongVersion_ReturnsEnoent) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "app1:9.9.9", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, Unlink_WithoutOpeningRoot_RemovesEntry) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(0, drv->Unlink(drv->ctx, 0, "app2:2.3.4"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, "app2", VFS_O_RDONLY));
}

TEST(vfs_registry_driver, Unlink_NamePrefix_ReturnsEnoent) {
    SeedTwo();
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Unlink(drv->ctx, 0, "app1x"));
}

/* State is per descriptor: opening and closing another one must not disturb an
 * install. Sharing it made the second write of an install answer -EBADF. */
TEST(vfs_registry_driver, Install_SurvivesAnotherDescriptorClosing) {
    SeedTwo();
    int install = drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(install >= 0);

    int other = OpenEntry("app1");
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, other));

    /* Still an install, and still named: the dummy refuses the write itself. */
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, install, "{}", 2));
}

/* An install must not be renamed by a later open. The write ref used to be one
 * field for the whole driver, so the second open retargeted the first. */
TEST(vfs_registry_driver, Install_KeepsItsRefAcrossAnotherOpen) {
    int first = drv->Open(drv->ctx, "newapp:1.0.0-1", VFS_O_WRONLY);
    int second = drv->Open(drv->ctx, "other:2.0.0", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(first >= 0);
    TEST_ASSERT_TRUE(second >= 0);
    TEST_ASSERT_TRUE(first != second);
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, first, "{}", 2));
}

/* The end-of-file flag is per descriptor. It was a function-static shared by
 * every reader, so one reader ended another's read. */
TEST(vfs_registry_driver, ReadEntry_EofIsPerDescriptor) {
    SeedTwo();
    int a = OpenEntry("app1");
    int b = OpenEntry("app2");

    uint8_t buf[160] = {0};
    TEST_ASSERT_TRUE(drv->Read(drv->ctx, a, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, a, buf, sizeof(buf)));
    /* b has read nothing yet, so it still has its descriptor to give. */
    TEST_ASSERT_TRUE(drv->Read(drv->ctx, b, buf, sizeof(buf)) > 0);
}

TEST(vfs_registry_driver, Open_TableExhausted_ReturnsEmfile) {
    SeedTwo();
    int last = 0;
    for (int i = 0; i < 32 && last >= 0; i++)
        last = drv->Open(drv->ctx, "app1", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EMFILE, last);
}

TEST(vfs_registry_driver, Close_UnopenedFd_ReturnsEbadf) {
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Close(drv->ctx, 3));
}

TEST(vfs_registry_driver, ReadDir_EntryFd_ReturnsEnotdir) {
    SeedTwo();
    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-ENOTDIR,
                          drv->ReadDir(drv->ctx, OpenEntry("app1"), buf,
                                       sizeof(buf), &cookie, &used));
}

/* A buffer too small for every entry must report the bytes it did write and
 * leave the cookie on the entry that did not fit. Reporting the whole buffer
 * hands the reader bytes this never wrote, and a reader that parses those as
 * an entry follows a length and a cookie that lead nowhere. */
TEST(vfs_registry_driver, ReadDir_ShortBuffer_ReportsWhatItWrote) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    /* Room for one entry only. */
    uint8_t buf[sizeof(vfs_dirent_t) + 16];
    uint64_t cookie = 0;
    size_t used = 0;

    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(used <= sizeof(buf));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "app1", 4));
    /* The second entry did not fit, thus the cookie still names it. */
    TEST_ASSERT_EQUAL_UINT64(1, cookie);
}

/* Every entry is served exactly once across the calls a short buffer forces,
 * and the walk ends. A cookie that skipped or repeated an entry would lose one
 * or never terminate. */
TEST(vfs_registry_driver, ReadDir_ShortBuffer_WalksEveryEntryOnce) {
    SeedTwo();
    drv->Open(drv->ctx, "/", VFS_O_RDONLY);

    uint8_t buf[sizeof(vfs_dirent_t) + 16];
    uint64_t cookie = 0;
    int seen1 = 0;
    int seen2 = 0;

    for (int calls = 0; calls < 8; calls++) {
        size_t used = 0;
        TEST_ASSERT_EQUAL_INT(
            0, drv->ReadDir(drv->ctx, 0, buf, sizeof(buf), &cookie, &used));
        if (used == 0)
            break; /* the walk is over */
        TEST_ASSERT_TRUE(used <= sizeof(buf));
        if (HasBytes(buf, used, "app1", 4))
            seen1++;
        if (HasBytes(buf, used, "app2", 4))
            seen2++;
    }

    TEST_ASSERT_EQUAL_INT(1, seen1);
    TEST_ASSERT_EQUAL_INT(1, seen2);
}

TEST_GROUP_RUNNER(vfs_registry_driver) {
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_ShortBuffer_ReportsWhatItWrote);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_ShortBuffer_WalksEveryEntryOnce);
    RUN_TEST_CASE(vfs_registry_driver,
                  Install_SurvivesAnotherDescriptorClosing);
    RUN_TEST_CASE(vfs_registry_driver, Install_KeepsItsRefAcrossAnotherOpen);
    RUN_TEST_CASE(vfs_registry_driver, ReadEntry_EofIsPerDescriptor);
    RUN_TEST_CASE(vfs_registry_driver, Open_TableExhausted_ReturnsEmfile);
    RUN_TEST_CASE(vfs_registry_driver, Close_UnopenedFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_EntryFd_ReturnsEnotdir);
    RUN_TEST_CASE(vfs_registry_driver, OpenRoot_LoadsEntries);
    RUN_TEST_CASE(vfs_registry_driver, OpenNullPath_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntry_ReturnsDistinctDescriptors);
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
    RUN_TEST_CASE(vfs_registry_driver, Read_FdBeyondEntries_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, Write_NullBuf_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_NullBuf_ReturnsEinval);
    RUN_TEST_CASE(vfs_registry_driver, ReadDir_NotOpened_ReturnsEbadf);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_ByNameVersion_RemovesEntry);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_RemovesEntry);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_Unknown_ReturnsEnoent);
    RUN_TEST_CASE(vfs_registry_driver, Close_AfterFailedStart_DoesNotFinalize);
    RUN_TEST_CASE(vfs_registry_driver,
                  WriteRegistry_FailedStart_LeavesWriteUnarmed);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntry_WithoutOpeningRoot_Resolves);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntry_NamePrefix_ReturnsEnoent);
    RUN_TEST_CASE(vfs_registry_driver, OpenEntry_WrongVersion_ReturnsEnoent);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_WithoutOpeningRoot_RemovesEntry);
    RUN_TEST_CASE(vfs_registry_driver, Unlink_NamePrefix_ReturnsEnoent);
}
