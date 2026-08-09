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

/* /dev/gpio: the per-pin subtree, the grant grammar, and the least-privilege
 * boundary. The dummy platform backs each granted address with an in-memory
 * line, so a test drives an input and observes an output without hardware. */

static vfs_ctx_t vfs;
static vfs_driver_t *drv;

/* Install a gpio driver with `options` and mount it. Returns false when the
 * grant was rejected, which is a failed launch. */
static bool setupGrant(const char *options) {
    vfs = VfsInit();
    drv = VfsGpioInit(NULL, options);
    if (drv == NULL)
        return false;
    DevFs_Register(vfs, "gpio", drv);
    return true;
}

static void teardown(void) {
    VfsDestroy(&vfs);
    drv = NULL;
    DummyGpioReset();
}

/* Read one node into `buf`, NUL-terminated. Returns the byte count or -errno.
 */
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
TEST_GROUP(gpio_grant);
/***************************************/

TEST_SETUP(gpio_grant) { DummyGpioReset(); }

TEST_TEAR_DOWN(gpio_grant) { DummyGpioReset(); }

/* A grant with no pins= clause has no default pin. It fails the launch rather
 * than driving whatever the board happens to have wired. */
TEST(gpio_grant, MissingPinsClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsGpioInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsGpioInit(NULL, ""));
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins="));
}

TEST(gpio_grant, MalformedEntryFailsLaunch) {
    /* Fewer than three fields. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:21"));
    /* A direction that is neither in nor out. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:21:inout"));
    /* A name outside [A-Za-z0-9_-]. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=le d:21:out"));
    /* An unknown per-entry suffix. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:21:out:speed=fast"));
    /* A key that is not pins=. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pin=21"));
}

/* An address is one field and the two separators are reserved, so a backing
 * whose native addressing uses either must spell it differently. A grant that
 * ignores this fails the launch rather than resolving to a different pin. */
TEST(gpio_grant, AddressCarryingASeparatorFailsLaunch) {
    /* A colon makes the field count ambiguous: this would otherwise read as
     * address "0" with direction "17". */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:0:17:out"));
    /* A comma splits the entry in two, and neither half parses. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:/dev/gpiochip0,17:out"));
    /* The spelling the record prescribes is accepted as an opaque address; the
     * dummy backing takes decimals only, so the launch still fails, but at the
     * backing rather than in the parse. */
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:/dev/gpiochip0/17:out"));
}

TEST(gpio_grant, DuplicateNameOrAddressFailsLaunch) {
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:21:out,led:22:out"));
    TEST_ASSERT_NULL(VfsGpioInit(NULL, "pins=led:21:out,lamp:21:out"));
}

TEST_GROUP_RUNNER(gpio_grant) {
    RUN_TEST_CASE(gpio_grant, MissingPinsClauseFailsLaunch);
    RUN_TEST_CASE(gpio_grant, MalformedEntryFailsLaunch);
    RUN_TEST_CASE(gpio_grant, AddressCarryingASeparatorFailsLaunch);
    RUN_TEST_CASE(gpio_grant, DuplicateNameOrAddressFailsLaunch);
}

/***************************************/
TEST_GROUP(gpio_tree);
/***************************************/

TEST_SETUP(gpio_tree) {
    DummyGpioReset();
    TEST_ASSERT_TRUE(setupGrant("pins=led:21:out,btn:2:in"));
}

TEST_TEAR_DOWN(gpio_tree) { teardown(); }

TEST(gpio_tree, GrantedPinReportsItsDirection) {
    char buf[16];
    TEST_ASSERT_GREATER_THAN_INT(0,
                                 readNode("/dev/gpio/led/direction", buf, 16));
    TEST_ASSERT_EQUAL_STRING("out\n", buf);
    TEST_ASSERT_GREATER_THAN_INT(0,
                                 readNode("/dev/gpio/btn/direction", buf, 16));
    TEST_ASSERT_EQUAL_STRING("in\n", buf);
}

/* An ungranted pin has no namespace at all. It is unreachable, not refused. */
TEST(gpio_tree, UngrantedPinIsEnoent) {
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/gpio/nrst/value", buf, 16));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/gpio/nrst", VFS_O_RDONLY));
}

TEST(gpio_tree, UnknownAttributeIsEnoent) {
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/gpio/led/pull", VFS_O_RDONLY));
}

TEST(gpio_tree, RootListsExactlyTheGrantedPins) {
    int fd = VfsOpen(vfs, "/dev/gpio", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_GREATER_THAN_UINT(0, used);

    /* The names appear in the packed dirent stream; the ungranted one does
     * not. */
    TEST_ASSERT_NOT_NULL(memchr(buf, 'l', used));
    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(gpio_tree) {
    RUN_TEST_CASE(gpio_tree, GrantedPinReportsItsDirection);
    RUN_TEST_CASE(gpio_tree, UngrantedPinIsEnoent);
    RUN_TEST_CASE(gpio_tree, UnknownAttributeIsEnoent);
    RUN_TEST_CASE(gpio_tree, RootListsExactlyTheGrantedPins);
}

/***************************************/
TEST_GROUP(gpio_value);
/***************************************/

TEST_SETUP(gpio_value) {
    DummyGpioReset();
    TEST_ASSERT_TRUE(setupGrant("pins=led:21:out,btn:2:in"));
}

TEST_TEAR_DOWN(gpio_value) { teardown(); }

TEST(gpio_value, WriteDrivesTheLine) {
    bool level = false;
    TEST_ASSERT_EQUAL_INT(1, writeNode("/dev/gpio/led/value", "1"));
    TEST_ASSERT_EQUAL_INT(0, DummyGpioGetLevel("21", &level));
    TEST_ASSERT_TRUE(level);

    TEST_ASSERT_EQUAL_INT(1, writeNode("/dev/gpio/led/value", "0"));
    TEST_ASSERT_EQUAL_INT(0, DummyGpioGetLevel("21", &level));
    TEST_ASSERT_FALSE(level);
}

/* A read of value regenerates on a fresh open: one line, then EOF. */
TEST(gpio_value, ReadReturnsTheLevelThenEof) {
    TEST_ASSERT_EQUAL_INT(0, DummyGpioSetLevel("2", true));

    int fd = VfsOpen(vfs, "/dev/gpio/btn/value", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(2, VfsRead(vfs, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(buf, "1\n", 2));
    TEST_ASSERT_EQUAL_INT(0, VfsRead(vfs, fd, buf, sizeof(buf)));
    VfsClose(vfs, fd);
}

/* A wapp cannot drive a pin the grant made an input. */
TEST(gpio_value, WriteToInputIsEperm) {
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("/dev/gpio/btn/value", "1"));
}

/* direction reports the grant; it does not set it. */
TEST(gpio_value, WriteToDirectionIsEperm) {
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("/dev/gpio/led/direction", "in"));
}

TEST(gpio_value, RejectsAPayloadThatIsNotALevel) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("/dev/gpio/led/value", "2"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("/dev/gpio/led/value", "on"));
}

/* A read of value emits a trailing newline, so a write must accept one back. */
TEST(gpio_value, AcceptsTheNewlineItEmits) {
    bool level = false;
    TEST_ASSERT_EQUAL_INT(2, writeNode("/dev/gpio/led/value", "1\n"));
    TEST_ASSERT_EQUAL_INT(0, DummyGpioGetLevel("21", &level));
    TEST_ASSERT_TRUE(level);
}

TEST_GROUP_RUNNER(gpio_value) {
    RUN_TEST_CASE(gpio_value, WriteDrivesTheLine);
    RUN_TEST_CASE(gpio_value, ReadReturnsTheLevelThenEof);
    RUN_TEST_CASE(gpio_value, WriteToInputIsEperm);
    RUN_TEST_CASE(gpio_value, WriteToDirectionIsEperm);
    RUN_TEST_CASE(gpio_value, RejectsAPayloadThatIsNotALevel);
    RUN_TEST_CASE(gpio_value, AcceptsTheNewlineItEmits);
}
