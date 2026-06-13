/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>

static wapp_t dummy_wapp;
static vfs_ctx_t vfs;

static void SetupCfgVfs(const char *content) {
    memset(&dummy_wapp, 0, sizeof(dummy_wapp));
    vfs = VfsInit();
    vfs_driver_t *drv = VfsConfigInit(&dummy_wapp, content);
    DevFs_Register(vfs, "cfg", drv);
}

/***************************************/
TEST_GROUP(vfs_config_init);
/***************************************/

TEST_SETUP(vfs_config_init) { memset(&dummy_wapp, 0, sizeof(dummy_wapp)); }
TEST_TEAR_DOWN(vfs_config_init) {}

TEST(vfs_config_init, NullWappReturnsNull) {
    TEST_ASSERT_NULL(VfsConfigInit(NULL, "content"));
}

TEST(vfs_config_init, NullOptReturnsNull) {
    TEST_ASSERT_NULL(VfsConfigInit(&dummy_wapp, NULL));
}

TEST(vfs_config_init, ValidArgsReturnsDriver) {
    vfs_driver_t *d = VfsConfigInit(&dummy_wapp, "data");
    TEST_ASSERT_NOT_NULL(d);
    d->Destroy(d);
}

TEST(vfs_config_init, EmptyContentAllowed) {
    vfs_driver_t *d = VfsConfigInit(&dummy_wapp, "");
    TEST_ASSERT_NOT_NULL(d);
    d->Destroy(d);
}

TEST_GROUP_RUNNER(vfs_config_init) {
    RUN_TEST_CASE(vfs_config_init, NullWappReturnsNull);
    RUN_TEST_CASE(vfs_config_init, NullOptReturnsNull);
    RUN_TEST_CASE(vfs_config_init, ValidArgsReturnsDriver);
    RUN_TEST_CASE(vfs_config_init, EmptyContentAllowed);
}

/***************************************/
TEST_GROUP(vfs_config_open);
/***************************************/

TEST_SETUP(vfs_config_open) { SetupCfgVfs("open-test"); }
TEST_TEAR_DOWN(vfs_config_open) { VfsDestroy(&vfs); }

TEST(vfs_config_open, ReadOnlySucceeds) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(vfs, fd);
}

TEST(vfs_config_open, WriteOnlyReturnsErofs) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EROFS, fd);
}

TEST(vfs_config_open, RdwrReturnsErofs) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(-EROFS, fd);
}

TEST_GROUP_RUNNER(vfs_config_open) {
    RUN_TEST_CASE(vfs_config_open, ReadOnlySucceeds);
    RUN_TEST_CASE(vfs_config_open, WriteOnlyReturnsErofs);
    RUN_TEST_CASE(vfs_config_open, RdwrReturnsErofs);
}

/***************************************/
TEST_GROUP(vfs_config_rw);
/***************************************/

static const char RW_CONTENT[] = "hello-config";

TEST_SETUP(vfs_config_rw) { SetupCfgVfs(RW_CONTENT); }
TEST_TEAR_DOWN(vfs_config_rw) { VfsDestroy(&vfs); }

TEST(vfs_config_rw, ReadReturnsContent) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[32] = {0};
    int n = VfsRead(vfs, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(RW_CONTENT) - 1), n);
    TEST_ASSERT_EQUAL_STRING_LEN(RW_CONTENT, buf, sizeof(RW_CONTENT) - 1);

    VfsClose(vfs, fd);
}

TEST(vfs_config_rw, SecondReadReturnsEof) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[32];
    VfsRead(vfs, fd, buf, sizeof(buf));
    int n = VfsRead(vfs, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, n);

    VfsClose(vfs, fd);
}

TEST(vfs_config_rw, WriteReturnsErofs) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    TEST_ASSERT_EQUAL_INT(-EROFS, VfsWrite(vfs, fd, "x", 1));

    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(vfs_config_rw) {
    RUN_TEST_CASE(vfs_config_rw, ReadReturnsContent);
    RUN_TEST_CASE(vfs_config_rw, SecondReadReturnsEof);
    RUN_TEST_CASE(vfs_config_rw, WriteReturnsErofs);
}

/***************************************/
TEST_GROUP(vfs_config_seek);
/***************************************/

static const char SEEK_CONTENT[] = "0123456789";

TEST_SETUP(vfs_config_seek) { SetupCfgVfs(SEEK_CONTENT); }
TEST_TEAR_DOWN(vfs_config_seek) { VfsDestroy(&vfs); }

TEST(vfs_config_seek, SeekSetThenRead) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 4, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(4, pos);

    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(3, VfsRead(vfs, fd, buf, 3));
    TEST_ASSERT_EQUAL_STRING_LEN("456", buf, 3);

    VfsClose(vfs, fd);
}

TEST(vfs_config_seek, SeekCurAdvances) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    VfsSeek(vfs, fd, 2, VFS_SEEK_SET, &pos);
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 3, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT(5, pos);

    VfsClose(vfs, fd);
}

TEST(vfs_config_seek, SeekEndIsFileSize) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 0, VFS_SEEK_END, &pos));
    TEST_ASSERT_EQUAL_INT(10, pos);

    VfsClose(vfs, fd);
}

TEST(vfs_config_seek, SeekPastEndClampsToLen) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 9999, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(10, pos);

    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(vfs_config_seek) {
    RUN_TEST_CASE(vfs_config_seek, SeekSetThenRead);
    RUN_TEST_CASE(vfs_config_seek, SeekCurAdvances);
    RUN_TEST_CASE(vfs_config_seek, SeekEndIsFileSize);
    RUN_TEST_CASE(vfs_config_seek, SeekPastEndClampsToLen);
}

/***************************************/
TEST_GROUP(vfs_config_stat);
/***************************************/

TEST_SETUP(vfs_config_stat) { SetupCfgVfs("stat-data"); }
TEST_TEAR_DOWN(vfs_config_stat) { VfsDestroy(&vfs); }

TEST(vfs_config_stat, IsRegularFileWithCorrectSize) {
    int fd = VfsOpen(vfs, "/dev/cfg", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(9, st.size);

    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(vfs_config_stat) {
    RUN_TEST_CASE(vfs_config_stat, IsRegularFileWithCorrectSize);
}
