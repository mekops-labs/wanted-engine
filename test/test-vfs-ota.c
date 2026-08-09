/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <dummy-fs.h>
#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* /dev/ota: the command grammar, the streaming image sink, and the ordering
 * that keeps an unverified image from becoming bootable. */

static vfs_ctx_t vfs;

static void setup(void) {
    DummyOtaReset();
    vfs = VfsInit();
    vfs_driver_t *drv = VfsOtaInit(NULL, NULL);
    DevFs_Register(vfs, "ota", drv);
}

static void teardown(void) {
    VfsDestroy(&vfs);
    DummyOtaReset();
}

static int writeNode(const char *path, const void *payload, size_t len) {
    int fd = VfsOpen(vfs, path, VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(vfs, fd, payload, len);
    VfsClose(vfs, fd);
    return n;
}

static int cmd(const char *verb) {
    return writeNode("/dev/ota", verb, strlen(verb));
}

static int readStatus(char *buf, size_t bufLen) {
    int fd = VfsOpen(vfs, "/dev/ota", VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(vfs, fd, buf, bufLen - 1);
    VfsClose(vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

/***************************************/
TEST_GROUP(ota_status);
/***************************************/

TEST_SETUP(ota_status) { setup(); }

TEST_TEAR_DOWN(ota_status) { teardown(); }

TEST(ota_status, ReportsAConfirmedSlotBeforeAnyUpdate) {
    char buf[256];
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "active_slot: a"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "status: confirmed"));
}

/* Absent, not empty: a reader must never take a blank value for the identity
 * of an image that is not there. */
TEST(ota_status, ReportsNoPendingDigestWithNothingStaged) {
    char buf[256];
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));
    TEST_ASSERT_NULL(strstr(buf, "pending_digest"));
}

TEST(ota_status, RejectsAnUnknownVerb) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, cmd("frobnicate"));
}

TEST_GROUP_RUNNER(ota_status) {
    RUN_TEST_CASE(ota_status, ReportsAConfirmedSlotBeforeAnyUpdate);
    RUN_TEST_CASE(ota_status, ReportsNoPendingDigestWithNothingStaged);
    RUN_TEST_CASE(ota_status, RejectsAnUnknownVerb);
}

/***************************************/
TEST_GROUP(ota_stage);
/***************************************/

TEST_SETUP(ota_stage) { setup(); }

TEST_TEAR_DOWN(ota_stage) { teardown(); }

/* An image is written to the inactive slot and only becomes bootable at
 * commit, which is what lets a caller verify a digest first. */
TEST(ota_stage, BeginWriteCommitStagesTheOtherSlot) {
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("begin"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/ota/slot", "FIRMWARE", 8));
    TEST_ASSERT_EQUAL_INT(-1, DummyOtaStagedLen());
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("commit"));
    TEST_ASSERT_EQUAL_INT(8, DummyOtaStagedLen());

    char buf[256];
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "status: unconfirmed"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "pending_slot: b"));
    /* The staged slot is not yet the one that runs. */
    TEST_ASSERT_EQUAL_INT('a', DummyOtaActiveSlot());
}

TEST(ota_stage, WriteBeforeBeginIsRefused) {
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("/dev/ota/slot", "X", 1));
}

/* A session left open holds the slot, so a second begin must not silently
 * discard the first. */
TEST(ota_stage, SecondBeginIsEbusy) {
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("begin"));
    TEST_ASSERT_EQUAL_INT(-EBUSY, cmd("begin"));
}

TEST(ota_stage, AbortReleasesTheSessionAndStagesNothing) {
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("begin"));
    TEST_ASSERT_GREATER_THAN_INT(0, writeNode("/dev/ota/slot", "BAD", 3));
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("abort"));
    TEST_ASSERT_EQUAL_INT(-1, DummyOtaStagedLen());
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("begin"));
}

TEST(ota_stage, AbortWithNoSessionSucceeds) {
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("abort"));
}

TEST_GROUP_RUNNER(ota_stage) {
    RUN_TEST_CASE(ota_stage, BeginWriteCommitStagesTheOtherSlot);
    RUN_TEST_CASE(ota_stage, WriteBeforeBeginIsRefused);
    RUN_TEST_CASE(ota_stage, SecondBeginIsEbusy);
    RUN_TEST_CASE(ota_stage, AbortReleasesTheSessionAndStagesNothing);
    RUN_TEST_CASE(ota_stage, AbortWithNoSessionSucceeds);
}

/***************************************/
TEST_GROUP(ota_activate);
/***************************************/

TEST_SETUP(ota_activate) { setup(); }

TEST_TEAR_DOWN(ota_activate) { teardown(); }

static void stageAnImage(void) {
    cmd("begin");
    writeNode("/dev/ota/slot", "FIRMWARE", 8);
    cmd("commit");
}

TEST(ota_activate, ConfirmMakesTheStagedSlotActive) {
    stageAnImage();
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("confirm"));
    TEST_ASSERT_EQUAL_INT('b', DummyOtaActiveSlot());

    char buf[256];
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "active_slot: b"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "status: confirmed"));
}

TEST(ota_activate, RollbackDropsThePendingSwap) {
    stageAnImage();
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("rollback"));
    TEST_ASSERT_EQUAL_INT('a', DummyOtaActiveSlot());

    char buf[256];
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "status: confirmed"));
}

/* The digest a status read reports for the staged slot is the one the running
 * image reports once that slot boots. That equality is what lets a control
 * plane confirm an update; the downloaded bytes' digest hashes a different
 * artifact and could never match. */
TEST(ota_activate, TheStagedDigestBecomesTheRunningDigest) {
    char buf[256];
    stageAnImage();
    TEST_ASSERT_GREATER_THAN_INT(0, readStatus(buf, sizeof(buf)));

    const char *at = strstr(buf, "pending_digest: ");
    TEST_ASSERT_NOT_NULL(at);
    char staged[FIRMWARE_DIGEST_HEX_LEN + 1];
    memcpy(staged, at + strlen("pending_digest: "), FIRMWARE_DIGEST_HEX_LEN);
    staged[FIRMWARE_DIGEST_HEX_LEN] = '\0';

    char running[FIRMWARE_DIGEST_HEX_LEN + 1];
    TEST_ASSERT_GREATER_THAN_INT(
        0, PlatformFirmwareDigest(running, sizeof(running)));
    TEST_ASSERT_TRUE(strcmp(staged, running) != 0);

    TEST_ASSERT_GREATER_THAN_INT(0, cmd("confirm"));
    TEST_ASSERT_GREATER_THAN_INT(
        0, PlatformFirmwareDigest(running, sizeof(running)));
    TEST_ASSERT_EQUAL_STRING(staged, running);
}

TEST(ota_activate, ConfirmWithNothingStagedSucceeds) {
    TEST_ASSERT_GREATER_THAN_INT(0, cmd("confirm"));
    TEST_ASSERT_EQUAL_INT('a', DummyOtaActiveSlot());
}

TEST_GROUP_RUNNER(ota_activate) {
    RUN_TEST_CASE(ota_activate, ConfirmMakesTheStagedSlotActive);
    RUN_TEST_CASE(ota_activate, RollbackDropsThePendingSwap);
    RUN_TEST_CASE(ota_activate, TheStagedDigestBecomesTheRunningDigest);
    RUN_TEST_CASE(ota_activate, ConfirmWithNothingStagedSucceeds);
}
