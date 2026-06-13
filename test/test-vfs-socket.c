/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <vfs-drivers.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_socket_driver — the "socket" driver (vfs-socket.c) driven through its
 * vtable, backed by the controllable dummy network mock (dummy-net.c).
 *
 * Options format parsed by VfsSocketInit: "<typechar> <addr> <port>", where
 * typechar is t/u (tcp/udp), T/U (secure tcp/udp), or b (bus).
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(vfs_socket_driver);

static vfs_driver_t *drv;

TEST_SETUP(vfs_socket_driver) {
    DummyNetReset();
    drv = NULL;
}

TEST_TEAR_DOWN(vfs_socket_driver) {
    if (drv && drv->Destroy)
        drv->Destroy(drv);
    drv = NULL;
}

/* ── VfsSocketInit parsing ──────────────────────────────────────────────── */

TEST(vfs_socket_driver, Init_Tcp_StreamFiletype) {
    drv = VfsSocketInit(NULL, "t 127.0.0.1 8080");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_STREAM, drv->filetype);
}

TEST(vfs_socket_driver, Init_Udp_DgramFiletype) {
    drv = VfsSocketInit(NULL, "u 10.0.0.2 53");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_DGRAM, drv->filetype);
}

TEST(vfs_socket_driver, Init_Bus_StreamFiletype) {
    drv = VfsSocketInit(NULL, "b mybus 0");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_STREAM, drv->filetype);
}

TEST(vfs_socket_driver, Init_NullOptions_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, NULL));
}

TEST(vfs_socket_driver, Init_TooFewFields_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "t"));
}

TEST(vfs_socket_driver, Init_BadTypeChar_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "z addr 1"));
}

TEST(vfs_socket_driver, Init_SecureTcp_RejectedWithoutSecureSupport) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "T addr 1"));
}

TEST(vfs_socket_driver, Init_SecureUdp_RejectedWithoutSecureSupport) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "U addr 1"));
}

/* ── Open / OpenAt / Close ──────────────────────────────────────────────── */

TEST(vfs_socket_driver, Open_AllocatesNetCtx) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_OpenFailure_ReturnsEconnaborted) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    DummyNetSetOpenFail(1);
    TEST_ASSERT_EQUAL_INT(-ECONNABORTED, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_WhenConnected_ReturnsZeroEarly) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    /* A read connects (mock connect succeeds), flipping the connected flag. */
    uint8_t buf[4];
    drv->Read(drv->ctx, 0, buf, sizeof(buf));
    /* Second open short-circuits on the connected flag. */
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_Reopen_ClosesOldCtx) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    /* Not connected yet, but netCtx is set → reopen path closes/frees old. */
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, OpenAt_DelegatesToOpen) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    TEST_ASSERT_EQUAL_INT(0, drv->OpenAt(drv->ctx, 0, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Close_ReturnsZero) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, 0));
}

/* ── Read / Write ───────────────────────────────────────────────────────── */

TEST(vfs_socket_driver, Read_ConnectsThenReceives) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSeedRecv((const uint8_t *)"hello", 5);

    uint8_t buf[8] = {0};
    int n = drv->Read(drv->ctx, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, 5);
}

TEST(vfs_socket_driver, Read_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ECONNREFUSED);

    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-ECONNREFUSED, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_socket_driver, Write_ConnectsThenSends) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);

    int n = drv->Write(drv->ctx, 0, "data", 4);
    TEST_ASSERT_EQUAL_INT(4, n);

    uint8_t sent[8] = {0};
    TEST_ASSERT_EQUAL_size_t(4, DummyNetGetSent(sent, sizeof(sent)));
    TEST_ASSERT_EQUAL_STRING_LEN("data", sent, 4);
}

TEST(vfs_socket_driver, Write_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-EHOSTUNREACH);
    TEST_ASSERT_EQUAL_INT(-EHOSTUNREACH, drv->Write(drv->ctx, 0, "x", 1));
}

/* ── Stat ───────────────────────────────────────────────────────────────── */

TEST(vfs_socket_driver, Stat_NullStat_ReturnsEinval) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Stat(drv->ctx, 0, NULL));
}

TEST(vfs_socket_driver, Stat_ReportsTypeAndPort) {
    drv = VfsSocketInit(NULL, "u 1.2.3.4 1234");

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, 0, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_DGRAM, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(1234, st.ino); /* ino carries the port */
}

/* ── Socket-specific ops ────────────────────────────────────────────────── */

TEST(vfs_socket_driver, SockAccept_NullNewFd_ReturnsEinval) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->SockAccept(drv->ctx, 0, 0, NULL));
}

TEST(vfs_socket_driver, SockAccept_ReturnsNewFd) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetAcceptResult(7);

    int newFd = -1;
    TEST_ASSERT_EQUAL_INT(7, drv->SockAccept(drv->ctx, 0, 0, &newFd));
    TEST_ASSERT_EQUAL_INT(7, newFd);
}

TEST(vfs_socket_driver, SockRecv_ConnectsThenReceives) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSeedRecv((const uint8_t *)"abc", 3);

    uint8_t buf[8] = {0};
    vfs_roflags_t oflags = 0;
    int n = drv->SockRecv(drv->ctx, 0, buf, sizeof(buf), 0, &oflags);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("abc", buf, 3);
}

TEST(vfs_socket_driver, SockRecv_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ECONNREFUSED);

    uint8_t buf[8];
    vfs_roflags_t oflags = 0;
    TEST_ASSERT_EQUAL_INT(-ECONNREFUSED,
        drv->SockRecv(drv->ctx, 0, buf, sizeof(buf), 0, &oflags));
}

TEST(vfs_socket_driver, SockSend_ConnectsThenSends) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);

    int n = drv->SockSend(drv->ctx, 0, "ping", 4, 0);
    TEST_ASSERT_EQUAL_INT(4, n);

    uint8_t sent[8] = {0};
    TEST_ASSERT_EQUAL_size_t(4, DummyNetGetSent(sent, sizeof(sent)));
    TEST_ASSERT_EQUAL_STRING_LEN("ping", sent, 4);
}

TEST(vfs_socket_driver, SockSend_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ENETDOWN);
    TEST_ASSERT_EQUAL_INT(-ENETDOWN, drv->SockSend(drv->ctx, 0, "x", 1, 0));
}

TEST(vfs_socket_driver, SockShutdown_ReturnsZero) {
    drv = VfsSocketInit(NULL, "t addr 8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, drv->SockShutdown(drv->ctx, 0, 0));
}

TEST_GROUP_RUNNER(vfs_socket_driver) {
    RUN_TEST_CASE(vfs_socket_driver, Init_Tcp_StreamFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_Udp_DgramFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_Bus_StreamFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_NullOptions_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_TooFewFields_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_BadTypeChar_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_SecureTcp_RejectedWithoutSecureSupport);
    RUN_TEST_CASE(vfs_socket_driver, Init_SecureUdp_RejectedWithoutSecureSupport);
    RUN_TEST_CASE(vfs_socket_driver, Open_AllocatesNetCtx);
    RUN_TEST_CASE(vfs_socket_driver, Open_OpenFailure_ReturnsEconnaborted);
    RUN_TEST_CASE(vfs_socket_driver, Open_WhenConnected_ReturnsZeroEarly);
    RUN_TEST_CASE(vfs_socket_driver, Open_Reopen_ClosesOldCtx);
    RUN_TEST_CASE(vfs_socket_driver, OpenAt_DelegatesToOpen);
    RUN_TEST_CASE(vfs_socket_driver, Close_ReturnsZero);
    RUN_TEST_CASE(vfs_socket_driver, Read_ConnectsThenReceives);
    RUN_TEST_CASE(vfs_socket_driver, Read_ConnectFailure_ReturnsError);
    RUN_TEST_CASE(vfs_socket_driver, Write_ConnectsThenSends);
    RUN_TEST_CASE(vfs_socket_driver, Write_ConnectFailure_ReturnsError);
    RUN_TEST_CASE(vfs_socket_driver, Stat_NullStat_ReturnsEinval);
    RUN_TEST_CASE(vfs_socket_driver, Stat_ReportsTypeAndPort);
    RUN_TEST_CASE(vfs_socket_driver, SockAccept_NullNewFd_ReturnsEinval);
    RUN_TEST_CASE(vfs_socket_driver, SockAccept_ReturnsNewFd);
    RUN_TEST_CASE(vfs_socket_driver, SockRecv_ConnectsThenReceives);
    RUN_TEST_CASE(vfs_socket_driver, SockRecv_ConnectFailure_ReturnsError);
    RUN_TEST_CASE(vfs_socket_driver, SockSend_ConnectsThenSends);
    RUN_TEST_CASE(vfs_socket_driver, SockSend_ConnectFailure_ReturnsError);
    RUN_TEST_CASE(vfs_socket_driver, SockShutdown_ReturnsZero);
}
