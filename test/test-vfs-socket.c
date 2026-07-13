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
 * Address format parsed by VfsSocketInit: a URL "<scheme>://<host>:<port>",
 * where scheme is tcp/udp (plain) or tcps/udps (secure TLS/DTLS).
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
    drv = VfsSocketInit(NULL, "tcp://127.0.0.1:8080");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_STREAM, drv->filetype);
}

TEST(vfs_socket_driver, Init_Udp_DgramFiletype) {
    drv = VfsSocketInit(NULL, "udp://10.0.0.2:53");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_DGRAM, drv->filetype);
}

TEST(vfs_socket_driver, Init_NullOptions_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, NULL));
}

TEST(vfs_socket_driver, Init_NoScheme_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "127.0.0.1:8080"));
}

TEST(vfs_socket_driver, Init_MissingPort_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "tcp://addr"));
}

TEST(vfs_socket_driver, Init_BadScheme_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "zzz://addr:1"));
}

TEST(vfs_socket_driver, Init_SecureTcp_RejectedWithoutSecureSupport) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "tcps://addr:1"));
}

TEST(vfs_socket_driver, Init_SecureUdp_RejectedWithoutSecureSupport) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "udps://addr:1"));
}

TEST(vfs_socket_driver, Init_Serial_CharDeviceFiletype) {
    drv = VfsSocketInit(NULL, "serial:///dev/ttyACM0");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, drv->filetype);
}

TEST(vfs_socket_driver, Init_Serial_EmptyPath_ReturnsNull) {
    TEST_ASSERT_NULL(VfsSocketInit(NULL, "serial://"));
}

TEST(vfs_socket_driver, Init_Serial_IgnoresColonInPath) {
    /* A path with no port to parse - unlike tcp/udp, a trailing ":n" in a
     * device path (unusual, but not disallowed) is just part of the path,
     * not a port to extract. */
    drv = VfsSocketInit(NULL, "serial:///dev/ttyACM0:0");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, drv->filetype);
}

/* ── Open / OpenAt / Close ──────────────────────────────────────────────── */

TEST(vfs_socket_driver, Open_AllocatesNetCtx) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_OpenFailure_ReturnsEconnaborted) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    DummyNetSetOpenFail(1);
    TEST_ASSERT_EQUAL_INT(-ECONNABORTED, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_WhenConnected_ReturnsZeroEarly) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    /* A read connects (mock connect succeeds), flipping the connected flag. */
    uint8_t buf[4];
    drv->Read(drv->ctx, 0, buf, sizeof(buf));
    /* Second open short-circuits on the connected flag. */
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Open_Reopen_ClosesOldCtx) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    /* Not connected yet, but netCtx is set → reopen path closes/frees old. */
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, OpenAt_DelegatesToOpen) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    TEST_ASSERT_EQUAL_INT(0, drv->OpenAt(drv->ctx, 0, "/", VFS_O_RDWR));
}

TEST(vfs_socket_driver, Close_ReturnsZero) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, 0));
}

/* ── Read / Write ───────────────────────────────────────────────────────── */

TEST(vfs_socket_driver, Read_ConnectsThenReceives) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSeedRecv((const uint8_t *)"hello", 5);

    uint8_t buf[8] = {0};
    int n = drv->Read(drv->ctx, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, 5);
}

TEST(vfs_socket_driver, Read_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ECONNREFUSED);

    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-ECONNREFUSED, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_socket_driver, Write_ConnectsThenSends) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);

    int n = drv->Write(drv->ctx, 0, "data", 4);
    TEST_ASSERT_EQUAL_INT(4, n);

    uint8_t sent[8] = {0};
    TEST_ASSERT_EQUAL_size_t(4, DummyNetGetSent(sent, sizeof(sent)));
    TEST_ASSERT_EQUAL_STRING_LEN("data", sent, 4);
}

TEST(vfs_socket_driver, Write_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-EHOSTUNREACH);
    TEST_ASSERT_EQUAL_INT(-EHOSTUNREACH, drv->Write(drv->ctx, 0, "x", 1));
}

/* ── Stat ───────────────────────────────────────────────────────────────── */

TEST(vfs_socket_driver, Stat_NullStat_ReturnsEinval) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Stat(drv->ctx, 0, NULL));
}

TEST(vfs_socket_driver, Stat_ReportsTypeAndPort) {
    drv = VfsSocketInit(NULL, "udp://1.2.3.4:1234");

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, 0, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_SOCKET_DGRAM, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(1234, st.ino); /* ino carries the port */
}

/* ── Socket-specific ops ────────────────────────────────────────────────── */

TEST(vfs_socket_driver, SockAccept_NullNewFd_ReturnsEinval) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->SockAccept(drv->ctx, 0, 0, NULL));
}

TEST(vfs_socket_driver, SockAccept_ReturnsNewFd) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetAcceptResult(7);

    int newFd = -1;
    TEST_ASSERT_EQUAL_INT(7, drv->SockAccept(drv->ctx, 0, 0, &newFd));
    TEST_ASSERT_EQUAL_INT(7, newFd);
}

TEST(vfs_socket_driver, SockRecv_ConnectsThenReceives) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSeedRecv((const uint8_t *)"abc", 3);

    uint8_t buf[8] = {0};
    vfs_roflags_t oflags = 0;
    int n = drv->SockRecv(drv->ctx, 0, buf, sizeof(buf), 0, &oflags);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("abc", buf, 3);
}

TEST(vfs_socket_driver, SockRecv_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ECONNREFUSED);

    uint8_t buf[8];
    vfs_roflags_t oflags = 0;
    TEST_ASSERT_EQUAL_INT(-ECONNREFUSED,
        drv->SockRecv(drv->ctx, 0, buf, sizeof(buf), 0, &oflags));
}

TEST(vfs_socket_driver, SockSend_ConnectsThenSends) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);

    int n = drv->SockSend(drv->ctx, 0, "ping", 4, 0);
    TEST_ASSERT_EQUAL_INT(4, n);

    uint8_t sent[8] = {0};
    TEST_ASSERT_EQUAL_size_t(4, DummyNetGetSent(sent, sizeof(sent)));
    TEST_ASSERT_EQUAL_STRING_LEN("ping", sent, 4);
}

TEST(vfs_socket_driver, SockSend_ConnectFailure_ReturnsError) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    DummyNetSetConnectResult(-ENETDOWN);
    TEST_ASSERT_EQUAL_INT(-ENETDOWN, drv->SockSend(drv->ctx, 0, "x", 1, 0));
}

TEST(vfs_socket_driver, SockShutdown_ReturnsZero) {
    drv = VfsSocketInit(NULL, "tcp://addr:8080");
    drv->Open(drv->ctx, "/", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, drv->SockShutdown(drv->ctx, 0, 0));
}

TEST_GROUP_RUNNER(vfs_socket_driver) {
    RUN_TEST_CASE(vfs_socket_driver, Init_Tcp_StreamFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_Udp_DgramFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_NullOptions_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_NoScheme_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_MissingPort_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_BadScheme_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_SecureTcp_RejectedWithoutSecureSupport);
    RUN_TEST_CASE(vfs_socket_driver, Init_SecureUdp_RejectedWithoutSecureSupport);
    RUN_TEST_CASE(vfs_socket_driver, Init_Serial_CharDeviceFiletype);
    RUN_TEST_CASE(vfs_socket_driver, Init_Serial_EmptyPath_ReturnsNull);
    RUN_TEST_CASE(vfs_socket_driver, Init_Serial_IgnoresColonInPath);
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
