/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <dummy-fs.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* /dev/uart: the per-port subtree, the grant grammar, and runtime line
 * reconfiguration. The dummy platform loops transmitted bytes back into the
 * receive ring, so a test covers short reads and the reconfiguration discard
 * without a peer on the wire. */

static vfs_ctx_t vfs;
static vfs_driver_t *drv;

static bool setupGrant(const char *options) {
    vfs = VfsInit();
    drv = VfsUartInit(NULL, options);
    if (drv == NULL)
        return false;
    DevFs_Register(vfs, "uart", drv);
    return true;
}

static void teardown(void) {
    VfsDestroy(&vfs);
    drv = NULL;
    DummyUartReset();
}

static int readNode(const char *path, char *buf, size_t bufLen) {
    int fd = VfsOpen(vfs, path, VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(vfs, fd, buf, bufLen - 1);
    VfsClose(vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int writeNode(const char *path, const char *payload) {
    int fd = VfsOpen(vfs, path, VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(vfs, fd, payload, strlen(payload));
    VfsClose(vfs, fd);
    return n;
}

/***************************************/
TEST_GROUP(uart_grant);
/***************************************/

TEST_SETUP(uart_grant) { DummyUartReset(); }

TEST_TEAR_DOWN(uart_grant) { DummyUartReset(); }

TEST(uart_grant, MissingPortFailsLaunch) {
    TEST_ASSERT_NULL(VfsUartInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsUartInit(NULL, ""));
    TEST_ASSERT_NULL(VfsUartInit(NULL, "baud=115200"));
}

TEST(uart_grant, MalformedClauseFailsLaunch) {
    /* A clause that is not key=value. */
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port"));
    /* A port name outside [A-Za-z0-9_-]. */
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=tty/0"));
    /* A frame format the grammar does not admit. */
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=1,format=8X1"));
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=1,format=8N3"));
    /* A baud rate that is not decimal. */
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=1,baud=fast"));
}

/* v1 grants exactly one port. */
TEST(uart_grant, SecondPortClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=1,port=2"));
}

/* The backing is exclusive: a second grant on a held port fails the launch. */
TEST(uart_grant, SecondGrantOnAHeldPortFailsLaunch) {
    TEST_ASSERT_TRUE(setupGrant("port=1"));
    TEST_ASSERT_NULL(VfsUartInit(NULL, "port=1"));
    teardown();
}

TEST_GROUP_RUNNER(uart_grant) {
    RUN_TEST_CASE(uart_grant, MissingPortFailsLaunch);
    RUN_TEST_CASE(uart_grant, MalformedClauseFailsLaunch);
    RUN_TEST_CASE(uart_grant, SecondPortClauseFailsLaunch);
    RUN_TEST_CASE(uart_grant, SecondGrantOnAHeldPortFailsLaunch);
}

/***************************************/
TEST_GROUP(uart_tree);
/***************************************/

TEST_SETUP(uart_tree) {
    DummyUartReset();
    TEST_ASSERT_TRUE(setupGrant("port=1,baud=57600,format=8E1"));
}

TEST_TEAR_DOWN(uart_tree) { teardown(); }

TEST(uart_tree, AttributesReportTheGrantsInitialSettings) {
    char buf[24];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/1/baud", buf, 24));
    TEST_ASSERT_EQUAL_STRING("57600\n", buf);
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/1/format", buf, 24));
    TEST_ASSERT_EQUAL_STRING("8E1\n", buf);
}

/* The default line configuration is 115200 8N1. */
TEST(uart_tree, DefaultsApplyWhenTheGrantOmitsThem) {
    teardown();
    TEST_ASSERT_TRUE(setupGrant("port=ttyA"));
    char buf[24];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/ttyA/baud", buf, 24));
    TEST_ASSERT_EQUAL_STRING("115200\n", buf);
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/ttyA/format", buf, 24));
    TEST_ASSERT_EQUAL_STRING("8N1\n", buf);
}

TEST(uart_tree, UngrantedPortIsEnoent) {
    char buf[24];
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/uart/2/data", buf, 24));
}

TEST(uart_tree, UnknownAttributeIsEnoent) {
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/uart/1/flush", VFS_O_RDONLY));
    /* v1 carries no ctl node: three attributes and no action to put on one. */
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/uart/1/ctl", VFS_O_WRONLY));
}

TEST_GROUP_RUNNER(uart_tree) {
    RUN_TEST_CASE(uart_tree, AttributesReportTheGrantsInitialSettings);
    RUN_TEST_CASE(uart_tree, DefaultsApplyWhenTheGrantOmitsThem);
    RUN_TEST_CASE(uart_tree, UngrantedPortIsEnoent);
    RUN_TEST_CASE(uart_tree, UnknownAttributeIsEnoent);
}

/***************************************/
TEST_GROUP(uart_data);
/***************************************/

TEST_SETUP(uart_data) {
    DummyUartReset();
    TEST_ASSERT_TRUE(setupGrant("port=1"));
}

TEST_TEAR_DOWN(uart_data) { teardown(); }

TEST(uart_data, WriteThenReadRoundTrips) {
    int wfd = VfsOpen(vfs, "/dev/uart/1/data", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(wfd >= 0);
    TEST_ASSERT_EQUAL_INT(5, VfsWrite(vfs, wfd, "hello", 5));
    VfsClose(vfs, wfd);

    int rfd = VfsOpen(vfs, "/dev/uart/1/data", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(rfd >= 0);
    char buf[16];
    TEST_ASSERT_EQUAL_INT(5, VfsRead(vfs, rfd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "hello", 5));
    VfsClose(vfs, rfd);
}

/* A read returns short rather than waiting to fill the caller's buffer. */
TEST(uart_data, ReadReturnsShort) {
    TEST_ASSERT_EQUAL_INT(2, writeNode("/dev/uart/1/data", "ab"));

    int rfd = VfsOpen(vfs, "/dev/uart/1/data", VFS_O_RDONLY);
    char buf[64];
    TEST_ASSERT_EQUAL_INT(2, VfsRead(vfs, rfd, buf, sizeof(buf)));
    VfsClose(vfs, rfd);
}

/* O_NONBLOCK is what makes a bounded read possible: the blocking read has no
 * wall-clock cap, so a wapp that wants a deadline runs its own clock. */
TEST(uart_data, NonBlockingReadOnAnEmptyPortIsEagain) {
    int fd = VfsOpen(vfs, "/dev/uart/1/data", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(vfs, fd, buf, sizeof(buf)));
    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(uart_data) {
    RUN_TEST_CASE(uart_data, WriteThenReadRoundTrips);
    RUN_TEST_CASE(uart_data, ReadReturnsShort);
    RUN_TEST_CASE(uart_data, NonBlockingReadOnAnEmptyPortIsEagain);
}

/***************************************/
TEST_GROUP(uart_reconfigure);
/***************************************/

TEST_SETUP(uart_reconfigure) {
    DummyUartReset();
    TEST_ASSERT_TRUE(setupGrant("port=1,baud=57600,format=8E1"));
}

TEST_TEAR_DOWN(uart_reconfigure) { teardown(); }

/* The bootloader sync and the framed channel share one wire and disagree on
 * both rate and parity, so both attributes are writable at runtime. */
TEST(uart_reconfigure, BaudAndFormatApplyImmediately) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/uart/1/baud", "921600"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/uart/1/format", "8N1"));

    uint32_t baud = 0;
    uint8_t databits = 0;
    uint8_t parity = 0;
    uint8_t stopbits = 0;
    TEST_ASSERT_EQUAL_INT(
        0, DummyUartGetLine(&baud, &databits, &parity, &stopbits));
    TEST_ASSERT_EQUAL_UINT32(921600, baud);
    TEST_ASSERT_EQUAL_UINT8(8, databits);
    TEST_ASSERT_EQUAL_UINT8('N', parity);
    TEST_ASSERT_EQUAL_UINT8(1, stopbits);

    char buf[24];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/1/baud", buf, 24));
    TEST_ASSERT_EQUAL_STRING("921600\n", buf);
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/uart/1/format", buf, 24));
    TEST_ASSERT_EQUAL_STRING("8N1\n", buf);
}

/* Bytes clocked in under the previous settings cannot be decoded under the new
 * ones, so a reconfiguration discards the receive buffer. */
TEST(uart_reconfigure, DiscardsTheReceiveBuffer) {
    TEST_ASSERT_EQUAL_INT(3, writeNode("/dev/uart/1/data", "abc"));
    TEST_ASSERT_EQUAL_INT(3, DummyUartRxLen());

    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/uart/1/baud", "115200"));
    TEST_ASSERT_EQUAL_INT(0, DummyUartRxLen());
}

/* A backing that cannot produce a rate must fail loudly. Silently selecting the
 * nearest achievable rate produces a link that looks configured and corrupts
 * data. */
TEST(uart_reconfigure, UnsupportedValueIsEinval) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("/dev/uart/1/baud", "7"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("/dev/uart/1/baud", "notanumber"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("/dev/uart/1/format", "9N1"));
}

/* A trailing newline is accepted, since a read of the node emits one. */
TEST(uart_reconfigure, AcceptsTheNewlineItEmits) {
    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/uart/1/baud", "115200\n"));
    uint32_t baud = 0;
    TEST_ASSERT_EQUAL_INT(0, DummyUartGetLine(&baud, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(115200, baud);
}

TEST_GROUP_RUNNER(uart_reconfigure) {
    RUN_TEST_CASE(uart_reconfigure, BaudAndFormatApplyImmediately);
    RUN_TEST_CASE(uart_reconfigure, DiscardsTheReceiveBuffer);
    RUN_TEST_CASE(uart_reconfigure, UnsupportedValueIsEinval);
    RUN_TEST_CASE(uart_reconfigure, AcceptsTheNewlineItEmits);
}
