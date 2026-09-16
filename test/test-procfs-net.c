/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "test-utils.h"

#include "dummy-fs.h"
#include <vfs-drivers.h>
#include <vfs-netfs.h>
#include <vfs-procfs.h>
#include <vfs.h>
#include <wanted-vfs-api.h>

/* Exercises /proc/net/<name> (vfs-procfs-net.c) against a real socket driver
 * registered the same way WantedInstallDriver would — no mocked ProcFS
 * internals, real VfsOpen/VfsRead/VfsReadDir. */

static vfs_ctx_t vfs;

TEST_GROUP(procfs_net);

TEST_SETUP(procfs_net) {
    DummyNetReset();
    vfs = VfsInit();
    ProcFs_RegisterDir(vfs, "net", &NetProcDirOps, false);
}

TEST_TEAR_DOWN(procfs_net) {
    VfsDestroy(&vfs);
    DummyNetReset();
}

TEST(procfs_net, EmptyWhenNoSocketsGranted) {
    int fd = VfsOpen(vfs, "/proc/net", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[64];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "manager", 7));
    VfsClose(vfs, fd);
}

TEST(procfs_net, EnumeratesOwnSocketsOnly) {
    NetFs_Register(vfs, "manager", VfsSocketInit(NULL, "tcp://addr:80"));

    int fd = VfsOpen(vfs, "/proc/net", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[64];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "manager", 7));
    VfsClose(vfs, fd);
}

/* The isolation property this feature exists for: a socket granted to one
 * wapp's own VFS context must never surface in a second, unrelated one. */
TEST(procfs_net, DoesNotLeakAnotherWappsSockets) {
    NetFs_Register(vfs, "manager", VfsSocketInit(NULL, "tcp://addr:80"));

    vfs_ctx_t other = VfsInit();
    ProcFs_RegisterDir(other, "net", &NetProcDirOps, false);

    int fd = VfsOpen(other, "/proc/net", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[64];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(other, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "manager", 7));
    VfsClose(other, fd);
    VfsDestroy(&other);
}

TEST(procfs_net, UnknownSocketName_ReturnsEnoent) {
    int fd = VfsOpen(vfs, "/proc/net/manager", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST(procfs_net, NotConnected_ReportsConnectedZero) {
    NetFs_Register(vfs, "manager", VfsSocketInit(NULL, "tcp://addr:80"));

    int fd = VfsOpen(vfs, "/proc/net/manager", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[64] = {0};
    int n = VfsRead(vfs, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("connected=0\n", buf);
    VfsClose(vfs, fd);
}

TEST(procfs_net, Connected_ReportsLocalAddr) {
    const vfs_driver_t *drv = VfsSocketInit(NULL, "tcp://addr:80");
    /* Connect the same driver instance before granting it — the lazy connect
     * on first I/O, exactly as a real wapp's own /net/manager traffic would
     * trigger it before ever reading /proc/net/manager. */
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    uint8_t rd[1];
    drv->Read(drv->ctx, 0, rd, sizeof(rd));
    DummyNetSetLocalAddr("203.0.113.4:51522");
    NetFs_Register(vfs, "manager", drv);

    int fd = VfsOpen(vfs, "/proc/net/manager", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[64] = {0};
    int n = VfsRead(vfs, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("connected=1\nlocal=203.0.113.4:51522\n", buf);
    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(procfs_net) {
    RUN_TEST_CASE(procfs_net, EmptyWhenNoSocketsGranted);
    RUN_TEST_CASE(procfs_net, EnumeratesOwnSocketsOnly);
    RUN_TEST_CASE(procfs_net, DoesNotLeakAnotherWappsSockets);
    RUN_TEST_CASE(procfs_net, UnknownSocketName_ReturnsEnoent);
    RUN_TEST_CASE(procfs_net, NotConnected_ReportsConnectedZero);
    RUN_TEST_CASE(procfs_net, Connected_ReportsLocalAddr);
}
