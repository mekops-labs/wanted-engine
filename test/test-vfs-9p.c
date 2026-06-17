/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>

#include <vfs.h>
#include <vfs-drivers.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_9p_driver — no-socket paths of the 9P driver (vfs-9p.c).
 *
 * The 9P driver is a protocol client over a real TCP/UDP socket: Open/Read/
 * Write/Stat/ReadDir/Unlink each do a round trip against a live peer, so they
 * cannot be unit-tested without a loopback 9P server. This group covers only
 * the deterministic paths that return before any socket I/O: the fd-range
 * guards, the pure _Seek math, the _OpenAt reject, driver init, and teardown.
 * The MAX_OPENED_FILES cap is 10, so fd 11 / -1 are out of range; fd 10 is NOT
 * (it is not > 10) and would reach the socket, so it is avoided here.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(vfs_9p_driver);

static vfs_driver_t *drv;

TEST_SETUP(vfs_9p_driver) {
    /* conf is never dialed: no test here calls _Open. */
    drv = Vfs9PInit(NULL, "tcp://127.0.0.1:9999");
}

TEST_TEAR_DOWN(vfs_9p_driver) {
    /* Exercises _Destroy's non-attached path (frees conf/ctx/driver). */
    if (drv && drv->Destroy)
        drv->Destroy(drv);
    drv = NULL;
}

TEST(vfs_9p_driver, Init_PopulatesVtable) {
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->ctx);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, drv->filetype);
    TEST_ASSERT_NOT_NULL(drv->Open);
    TEST_ASSERT_NOT_NULL(drv->OpenAt);
    TEST_ASSERT_NOT_NULL(drv->Close);
    TEST_ASSERT_NOT_NULL(drv->Stat);
    TEST_ASSERT_NOT_NULL(drv->Read);
    TEST_ASSERT_NOT_NULL(drv->Write);
    TEST_ASSERT_NOT_NULL(drv->Seek);
    TEST_ASSERT_NOT_NULL(drv->ReadDir);
    TEST_ASSERT_NOT_NULL(drv->Unlink);
    TEST_ASSERT_NOT_NULL(drv->Destroy);
}

TEST(vfs_9p_driver, OpenAt_RejectsUnsupported) {
    TEST_ASSERT_EQUAL_INT(-ENOTSUP,
                          drv->OpenAt(drv->ctx, 0, "anything", VFS_O_RDWR));
}

TEST(vfs_9p_driver, Seek_Set) {
    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, drv->Seek(drv->ctx, 1, 100, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(100, pos);
}

TEST(vfs_9p_driver, Seek_Cur_Accumulates) {
    long pos = -1;
    drv->Seek(drv->ctx, 1, 100, VFS_SEEK_SET, &pos);
    TEST_ASSERT_EQUAL_INT(0, drv->Seek(drv->ctx, 1, 10, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT(110, pos);
}

TEST(vfs_9p_driver, Seek_End_LeavesOffsetUnchanged) {
    long pos = -1;
    drv->Seek(drv->ctx, 1, 42, VFS_SEEK_SET, &pos);
    /* SEEK_END is a no-op in this driver: it just reports the current offset. */
    TEST_ASSERT_EQUAL_INT(0, drv->Seek(drv->ctx, 1, 999, VFS_SEEK_END, &pos));
    TEST_ASSERT_EQUAL_INT(42, pos);
}

TEST(vfs_9p_driver, Seek_BadWhence_ReturnsEinval) {
    long pos = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL,
        drv->Seek(drv->ctx, 1, 0, (vfs_whence_t)99, &pos));
}

TEST(vfs_9p_driver, Seek_BadFd_ReturnsEbadf) {
    long pos = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Seek(drv->ctx, 11, 0, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Seek(drv->ctx, -1, 0, VFS_SEEK_SET, &pos));
}

TEST(vfs_9p_driver, Close_Fd0_IsNoOpSuccess) {
    /* fd 0 is the attach root: Close skips the clunk round trip and succeeds. */
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, 0));
}

TEST(vfs_9p_driver, Close_BadFd_ReturnsEbadf) {
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Close(drv->ctx, 11));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Close(drv->ctx, -1));
}

TEST(vfs_9p_driver, Stat_BadFd_ReturnsEbadf) {
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Stat(drv->ctx, 11, &st));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Stat(drv->ctx, -1, &st));
}

TEST(vfs_9p_driver, Read_BadFd_ReturnsEbadf) {
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Read(drv->ctx, 11, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Read(drv->ctx, -1, buf, sizeof(buf)));
}

TEST(vfs_9p_driver, Write_BadFd_ReturnsEbadf) {
    uint8_t buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Write(drv->ctx, 11, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Write(drv->ctx, -1, buf, sizeof(buf)));
}

TEST(vfs_9p_driver, ReadDir_BadFd_ReturnsEbadf) {
    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF,
        drv->ReadDir(drv->ctx, 11, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_EQUAL_INT(-EBADF,
        drv->ReadDir(drv->ctx, -1, buf, sizeof(buf), &cookie, &used));
}

TEST(vfs_9p_driver, Unlink_BadFd_ReturnsEbadf) {
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Unlink(drv->ctx, 11, "/x"));
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Unlink(drv->ctx, -1, "/x"));
}

TEST_GROUP_RUNNER(vfs_9p_driver) {
    RUN_TEST_CASE(vfs_9p_driver, Init_PopulatesVtable);
    RUN_TEST_CASE(vfs_9p_driver, OpenAt_RejectsUnsupported);
    RUN_TEST_CASE(vfs_9p_driver, Seek_Set);
    RUN_TEST_CASE(vfs_9p_driver, Seek_Cur_Accumulates);
    RUN_TEST_CASE(vfs_9p_driver, Seek_End_LeavesOffsetUnchanged);
    RUN_TEST_CASE(vfs_9p_driver, Seek_BadWhence_ReturnsEinval);
    RUN_TEST_CASE(vfs_9p_driver, Seek_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, Close_Fd0_IsNoOpSuccess);
    RUN_TEST_CASE(vfs_9p_driver, Close_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, Stat_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, Read_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, Write_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, ReadDir_BadFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_9p_driver, Unlink_BadFd_ReturnsEbadf);
}
