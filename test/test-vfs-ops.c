/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-procfs.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* ── Shared helpers ──────────────────────────────────────────────────────── */

static int _ProcReadVersion(vfs_ctx_t c, void *buf, size_t len) {
    (void)c;
    const char data[] = "1.0";
    size_t n = sizeof(data) - 1;
    if (n > len) n = len;
    memcpy(buf, data, n);
    return (int)n;
}

/* Minimal tar layer with a single file "data.bin" containing "0123456789". */
static uint8_t tar_layer[512 * 4];

static vfs_ctx_t BuildTarVfs(void) {
    memset(tar_layer, 0, sizeof(tar_layer));
    TarHeader(tar_layer, "data.bin", 10, '0');
    memcpy(tar_layer + 512, "0123456789", 10);

    uint8_t *layers[1] = {tar_layer};
    size_t lens[1] = {sizeof(tar_layer)};
    vfs_tarfs_ctx_t *t = TarFsInit(layers, lens, 1);
    vfs_ctx_t v = VfsInit();
    VfsAttachTarfs(v, t);
    return v;
}

/***************************************/
TEST_GROUP(vfs_stat_ops);
/***************************************/

static vfs_ctx_t vfs_stat;

TEST_SETUP(vfs_stat_ops) { vfs_stat = BuildTarVfs(); }

TEST_TEAR_DOWN(vfs_stat_ops) { VfsDestroy(&vfs_stat); }

TEST(vfs_stat_ops, InvalidFdReturnsEbadf) {
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsStat(vfs_stat, 99, &st));
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsStat(vfs_stat, -1, &st));
}

TEST(vfs_stat_ops, TarfsFileIsRegularFile) {
    int fd = VfsOpen(vfs_stat, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs_stat, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(10, st.size);

    VfsClose(vfs_stat, fd);
}

TEST(vfs_stat_ops, ProcfsEntryIsRegularFile) {
    ProcFs_Register(vfs_stat, "version", _ProcReadVersion, false);
    int fd = VfsOpen(vfs_stat, "/proc/version", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs_stat, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);

    VfsClose(vfs_stat, fd);
}

TEST(vfs_stat_ops, DevfsNullIsCharacterDevice) {
    vfs_driver_t *null_drv = VfsNullInit(NULL, NULL);
    DevFs_Register(vfs_stat, "null", null_drv);

    int fd = VfsOpen(vfs_stat, "/dev/null", VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs_stat, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, st.filetype);

    VfsClose(vfs_stat, fd);
}

TEST_GROUP_RUNNER(vfs_stat_ops) {
    RUN_TEST_CASE(vfs_stat_ops, InvalidFdReturnsEbadf);
    RUN_TEST_CASE(vfs_stat_ops, TarfsFileIsRegularFile);
    RUN_TEST_CASE(vfs_stat_ops, ProcfsEntryIsRegularFile);
    RUN_TEST_CASE(vfs_stat_ops, DevfsNullIsCharacterDevice);
}

/***************************************/
TEST_GROUP(vfs_seek_ops);
/***************************************/

static vfs_ctx_t vfs_seek;

TEST_SETUP(vfs_seek_ops) { vfs_seek = BuildTarVfs(); }

TEST_TEAR_DOWN(vfs_seek_ops) { VfsDestroy(&vfs_seek); }

TEST(vfs_seek_ops, InvalidFdReturnsEbadf) {
    long pos = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsSeek(vfs_seek, 99, 0, VFS_SEEK_SET, &pos));
}

TEST(vfs_seek_ops, NullPosReturnsEinval) {
    int fd = VfsOpen(vfs_seek, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsSeek(vfs_seek, fd, 0, VFS_SEEK_SET, NULL));
    VfsClose(vfs_seek, fd);
}

TEST(vfs_seek_ops, SeekSetAndRead) {
    int fd = VfsOpen(vfs_seek, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs_seek, fd, 4, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(4, pos);

    char buf[4] = {0};
    int n = VfsRead(vfs_seek, fd, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("456", buf, 3);

    VfsClose(vfs_seek, fd);
}

TEST(vfs_seek_ops, SeekCurAdvances) {
    int fd = VfsOpen(vfs_seek, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    VfsSeek(vfs_seek, fd, 2, VFS_SEEK_SET, &pos);
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs_seek, fd, 3, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT(5, pos);

    VfsClose(vfs_seek, fd);
}

TEST(vfs_seek_ops, SeekEndClampsToFileSize) {
    int fd = VfsOpen(vfs_seek, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs_seek, fd, 0, VFS_SEEK_END, &pos));
    TEST_ASSERT_EQUAL_INT(10, pos);

    VfsClose(vfs_seek, fd);
}

TEST(vfs_seek_ops, ProcfdSeekNotSupported) {
    ProcFs_Register(vfs_seek, "ver", _ProcReadVersion, false);
    int fd = VfsOpen(vfs_seek, "/proc/ver", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    long pos = 0;
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, VfsSeek(vfs_seek, fd, 0, VFS_SEEK_SET, &pos));

    VfsClose(vfs_seek, fd);
}

TEST_GROUP_RUNNER(vfs_seek_ops) {
    RUN_TEST_CASE(vfs_seek_ops, InvalidFdReturnsEbadf);
    RUN_TEST_CASE(vfs_seek_ops, NullPosReturnsEinval);
    RUN_TEST_CASE(vfs_seek_ops, SeekSetAndRead);
    RUN_TEST_CASE(vfs_seek_ops, SeekCurAdvances);
    RUN_TEST_CASE(vfs_seek_ops, SeekEndClampsToFileSize);
    RUN_TEST_CASE(vfs_seek_ops, ProcfdSeekNotSupported);
}

/***************************************/
TEST_GROUP(vfs_readdir_root);
/***************************************/

static vfs_ctx_t vfs_rd;

TEST_SETUP(vfs_readdir_root) { vfs_rd = BuildTarVfs(); }

TEST_TEAR_DOWN(vfs_readdir_root) { VfsDestroy(&vfs_rd); }

TEST(vfs_readdir_root, InvalidFdReturnsEbadf) {
    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsReadDir(vfs_rd, 99, buf, sizeof(buf), &cookie, &used));
}

TEST(vfs_readdir_root, NullArgsReturnEinval) {
    int fd = VfsOpen(vfs_rd, "/", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;

    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsReadDir(vfs_rd, fd, NULL, sizeof(buf), &cookie, &used));
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsReadDir(vfs_rd, fd, buf,  sizeof(buf), NULL,    &used));
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsReadDir(vfs_rd, fd, buf,  sizeof(buf), &cookie, NULL));

    VfsClose(vfs_rd, fd);
}

TEST(vfs_readdir_root, RootListsDataBinAndMountDirs) {
    int fd = VfsOpen(vfs_rd, "/", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int r = VfsReadDir(vfs_rd, fd, buf, sizeof(buf), &cookie, &used);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_TRUE(used > 0);

    /* TarFS file. */
    TEST_ASSERT_TRUE(HasBytes(buf, used, "data.bin", 8));
    /* Mount-table directories (dev, net, proc). */
    TEST_ASSERT_TRUE(HasBytes(buf, used, "dev",  3));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "net",  3));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "proc", 4));

    VfsClose(vfs_rd, fd);
}

TEST(vfs_readdir_root, SubdirListsOnlyTarfsChildren) {
    /* Add a subdirectory to the tarfs to test non-root readdir path. */
    memset(tar_layer, 0, sizeof(tar_layer));
    TarHeader(tar_layer,         "sub/file.txt", 3, '0');
    memcpy(tar_layer + 512,      "txt", 3);

    uint8_t *layers[1] = {tar_layer};
    size_t lens[1] = {sizeof(tar_layer)};
    vfs_tarfs_ctx_t *t = TarFsInit(layers, lens, 1);
    VfsAttachTarfs(vfs_rd, t);

    int fd = VfsOpen(vfs_rd, "/sub", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, VfsReadDir(vfs_rd, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);

    /* Only "file.txt" should appear — no mount dirs inside a subdir. */
    TEST_ASSERT_TRUE(HasBytes(buf, used, "file.txt", 8));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "dev",  3));

    VfsClose(vfs_rd, fd);
}

TEST_GROUP_RUNNER(vfs_readdir_root) {
    RUN_TEST_CASE(vfs_readdir_root, InvalidFdReturnsEbadf);
    RUN_TEST_CASE(vfs_readdir_root, NullArgsReturnEinval);
    RUN_TEST_CASE(vfs_readdir_root, RootListsDataBinAndMountDirs);
    RUN_TEST_CASE(vfs_readdir_root, SubdirListsOnlyTarfsChildren);
}

/***************************************/
TEST_GROUP(vfs_privileged);
/***************************************/

static vfs_ctx_t vfs_priv;

TEST_SETUP(vfs_privileged) {
    vfs_priv = VfsInit();
    ProcFs_Register(vfs_priv, "pub",  _ProcReadVersion, false);
    ProcFs_Register(vfs_priv, "priv", _ProcReadVersion, true);
}

TEST_TEAR_DOWN(vfs_privileged) { VfsDestroy(&vfs_priv); }

TEST(vfs_privileged, DefaultIsUnprivileged) {
    TEST_ASSERT_FALSE(vfs_priv->privileged);
    int fd = VfsOpen(vfs_priv, "/proc/priv", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EACCES, fd);
}

TEST(vfs_privileged, SetPrivilegedAllowsAccess) {
    VfsSetPrivileged(vfs_priv, true);
    int fd = VfsOpen(vfs_priv, "/proc/priv", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(vfs_priv, fd);
}

TEST(vfs_privileged, PublicEntryAlwaysAccessible) {
    VfsSetPrivileged(vfs_priv, false);
    int fd = VfsOpen(vfs_priv, "/proc/pub", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(vfs_priv, fd);
}

TEST_GROUP_RUNNER(vfs_privileged) {
    RUN_TEST_CASE(vfs_privileged, DefaultIsUnprivileged);
    RUN_TEST_CASE(vfs_privileged, SetPrivilegedAllowsAccess);
    RUN_TEST_CASE(vfs_privileged, PublicEntryAlwaysAccessible);
}
