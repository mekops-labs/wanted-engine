/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dummy-fs.h>
#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-haptic.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* /dev/haptic: the motor subtree, the grant grammar, the verbs and the board
 * seam. The state-only pin backing stays busy for a pattern's length on the
 * monotonic clock, so a test steps time with DummyClockAdvance. */

#define NS_PER_MS 1000000ULL
#define MAIN_PIN "motors=main:12:onoff"

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t other;

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsHapticInit(NULL, options);
    if (p->drv == NULL) {
        VfsDestroy(&p->vfs);
        return false;
    }
    DevFs_Register(p->vfs, "haptic", p->drv);
    return true;
}

static void detach(peer_t *p) {
    if (p->drv != NULL)
        VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static void teardown(void) {
    detach(&other);
    detach(&owner);
    HapticDevicesReset();
}

static void advanceMs(unsigned ms) { DummyClockAdvance(ms * NS_PER_MS); }

/* `path` is relative to /dev/haptic. */
static const char *fullPath(const char *path, char *out, size_t outLen) {
    snprintf(out, outLen, "/dev/haptic%s%s", path[0] != '\0' ? "/" : "", path);
    return out;
}

static int readVia(peer_t *p, const char *path, char *buf, size_t bufLen) {
    char full[64];
    int fd = VfsOpen(p->vfs, fullPath(path, full, sizeof(full)), VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(p->vfs, fd, buf, bufLen - 1);
    VfsClose(p->vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int writeVia(peer_t *p, const char *path, const char *payload) {
    char full[64];
    int fd = VfsOpen(p->vfs, fullPath(path, full, sizeof(full)), VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(p->vfs, fd, payload, strlen(payload));
    VfsClose(p->vfs, fd);
    return n;
}

static int ctl(const char *payload) {
    return writeVia(&owner, "main/ctl", payload);
}

static void sendCtl(const char *payload) {
    TEST_ASSERT_EQUAL_INT((int)strlen(payload), ctl(payload));
}

/* The busy node: 1, 0, or a negative errno. */
static int busy(peer_t *p) {
    char buf[8];
    int n = readVia(p, "main/busy", buf, sizeof(buf));
    return n < 0 ? n : atoi(buf);
}

/* The names in a directory, joined by ',' into `out`. Returns 0 or -errno. */
static int listDir(peer_t *p, const char *path, char *out, size_t outLen) {
    char full[64];
    int fd = VfsOpen(p->vfs, fullPath(path, full, sizeof(full)), VFS_O_RDONLY);
    if (fd < 0)
        return fd;

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int rc = VfsReadDir(p->vfs, fd, buf, sizeof(buf), &cookie, &used);
    VfsClose(p->vfs, fd);
    if (rc < 0)
        return rc;

    out[0] = '\0';
    size_t off = 0;
    while (off + sizeof(vfs_dirent_t) <= used) {
        vfs_dirent_t ent;
        memcpy(&ent, buf + off, sizeof(ent));
        off += sizeof(ent);
        if (out[0] != '\0')
            strncat(out, ",", outLen - strlen(out) - 1);
        strncat(out, (const char *)buf + off,
                (ent.d_namlen < outLen - strlen(out) - 1)
                    ? ent.d_namlen
                    : outLen - strlen(out) - 1);
        off += ent.d_namlen;
    }
    return 0;
}

/***************************************/
TEST_GROUP(haptic_grant);
/***************************************/

TEST_SETUP(haptic_grant) {}

TEST_TEAR_DOWN(haptic_grant) { teardown(); }

TEST(haptic_grant, MissingMotorsClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, ""));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors="));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motor=main:12:onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=observe"));
}

TEST(haptic_grant, MalformedEntryFailsLaunch) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff:x"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:pwm"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=:12:onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main::onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=m n:12:onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=abcdefghijklmnop:12:onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff,,b:13:onoff"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff,"));
}

TEST(haptic_grant, UnknownBoardNameFailsLaunch) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=drv"));
}

TEST(haptic_grant, RepeatedNameFailsLaunch) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff,main:13:onoff"));
}

TEST(haptic_grant, RepeatedAddressFailsLaunchAndFreesTheFirst) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=a:12:onoff,b:12:onoff"));
    TEST_ASSERT_TRUE(attach(&owner, "motors=a:12:onoff"));
}

TEST(haptic_grant, ObserveWithoutAnExistingDeviceFailsLaunch) {
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff,observe"));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main,observe,observe"));
}

TEST(haptic_grant, ReaddirListsExactlyTheGrantedMotors) {
    char names[64];
    TEST_ASSERT_TRUE(attach(&owner, "motors=main:12:onoff,aux:13:onoff"));
    TEST_ASSERT_EQUAL_INT(0, listDir(&owner, "", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("main,aux", names);
    TEST_ASSERT_EQUAL_INT(0, listDir(&owner, "main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("ctl,effects,busy", names);
}

TEST(haptic_grant, AnUngrantedNameIsAbsent) {
    char buf[32];
    TEST_ASSERT_TRUE(attach(&owner, MAIN_PIN));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "aux/busy", buf, 32));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "main/nope", buf, 32));
}

TEST_GROUP_RUNNER(haptic_grant) {
    RUN_TEST_CASE(haptic_grant, MissingMotorsClauseFailsLaunch);
    RUN_TEST_CASE(haptic_grant, MalformedEntryFailsLaunch);
    RUN_TEST_CASE(haptic_grant, UnknownBoardNameFailsLaunch);
    RUN_TEST_CASE(haptic_grant, RepeatedNameFailsLaunch);
    RUN_TEST_CASE(haptic_grant, RepeatedAddressFailsLaunchAndFreesTheFirst);
    RUN_TEST_CASE(haptic_grant, ObserveWithoutAnExistingDeviceFailsLaunch);
    RUN_TEST_CASE(haptic_grant, ReaddirListsExactlyTheGrantedMotors);
    RUN_TEST_CASE(haptic_grant, AnUngrantedNameIsAbsent);
}

/***************************************/
TEST_GROUP(haptic_ctl);
/***************************************/

TEST_SETUP(haptic_ctl) { TEST_ASSERT_TRUE(attach(&owner, MAIN_PIN)); }

TEST_TEAR_DOWN(haptic_ctl) { teardown(); }

TEST(haptic_ctl, EffectsListsTheFourNames) {
    char buf[64];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/effects", buf, 64));
    TEST_ASSERT_EQUAL_STRING("click\ndouble\nbuzz\nalert\n", buf);
}

TEST(haptic_ctl, AnIdleMotorReadsNotBusy) {
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, PlayStartsAtOnceAndBusyFollowsItsLength) {
    sendCtl("play buzz");
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    advanceMs(140);
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    advanceMs(20);
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, DoubleRunsThreeSegments) {
    sendCtl("play double");
    advanceMs(100);
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    advanceMs(20);
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, EveryEffectNameIsAccepted) {
    sendCtl("play click");
    sendCtl("play double");
    sendCtl("play buzz");
    sendCtl("play alert");
}

TEST(haptic_ctl, UnknownEffectIsRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("play thump"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("play"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("play click now"));
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, PulseRunsForItsDuration) {
    sendCtl("pulse 300");
    advanceMs(290);
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    advanceMs(20);
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, PulseIsCappedAtTwoThousandMilliseconds) {
    sendCtl("pulse 2000");
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("pulse 2001"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("pulse 0"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("pulse -5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("pulse"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("pulse 1x"));
}

TEST(haptic_ctl, ANewCommandReplacesTheRunningOne) {
    sendCtl("pulse 1000");
    sendCtl("play click");
    advanceMs(30);
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, ANewCommandRestartsTheClock) {
    sendCtl("pulse 100");
    advanceMs(80);
    sendCtl("pulse 100");
    advanceMs(80);
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    advanceMs(30);
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST(haptic_ctl, StopEndsAnythingAndIsNeverAnError) {
    sendCtl("stop");
    sendCtl("pulse 1000");
    sendCtl("stop");
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("stop now"));
}

TEST(haptic_ctl, UnknownVerbsAndLongLinesAreRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("vibrate"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl(""));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, ctl("play aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
}

TEST(haptic_ctl, ATrailingNewlineIsAccepted) { sendCtl("play click\n"); }

TEST(haptic_ctl, ReadOnlyNodesRefuseWritesAndCtlRefusesReads) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EPERM, writeVia(&owner, "main/busy", "1"));
    TEST_ASSERT_EQUAL_INT(-EPERM, writeVia(&owner, "main/effects", "x"));
    TEST_ASSERT_EQUAL_INT(-EPERM, readVia(&owner, "main/ctl", buf, 8));
}

TEST(haptic_ctl, TheMotorStopsWhenTheWriterGoes) {
    sendCtl("pulse 1000");
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, MAIN_PIN));
    TEST_ASSERT_EQUAL_INT(0, busy(&owner));
}

TEST_GROUP_RUNNER(haptic_ctl) {
    RUN_TEST_CASE(haptic_ctl, EffectsListsTheFourNames);
    RUN_TEST_CASE(haptic_ctl, AnIdleMotorReadsNotBusy);
    RUN_TEST_CASE(haptic_ctl, PlayStartsAtOnceAndBusyFollowsItsLength);
    RUN_TEST_CASE(haptic_ctl, DoubleRunsThreeSegments);
    RUN_TEST_CASE(haptic_ctl, EveryEffectNameIsAccepted);
    RUN_TEST_CASE(haptic_ctl, UnknownEffectIsRejected);
    RUN_TEST_CASE(haptic_ctl, PulseRunsForItsDuration);
    RUN_TEST_CASE(haptic_ctl, PulseIsCappedAtTwoThousandMilliseconds);
    RUN_TEST_CASE(haptic_ctl, ANewCommandReplacesTheRunningOne);
    RUN_TEST_CASE(haptic_ctl, ANewCommandRestartsTheClock);
    RUN_TEST_CASE(haptic_ctl, StopEndsAnythingAndIsNeverAnError);
    RUN_TEST_CASE(haptic_ctl, UnknownVerbsAndLongLinesAreRejected);
    RUN_TEST_CASE(haptic_ctl, ATrailingNewlineIsAccepted);
    RUN_TEST_CASE(haptic_ctl, ReadOnlyNodesRefuseWritesAndCtlRefusesReads);
    RUN_TEST_CASE(haptic_ctl, TheMotorStopsWhenTheWriterGoes);
}

/***************************************/
TEST_GROUP(haptic_board);
/***************************************/

typedef struct fake_t {
    unsigned plays;
    unsigned lastEffect;
    unsigned pulses;
    unsigned lastMs;
    unsigned stops;
    bool busy;
    int failPlay;
} fake_t;

static fake_t fake;

static int fakePlay(void *ctx, unsigned effect) {
    (void)ctx;
    if (fake.failPlay != 0)
        return fake.failPlay;
    fake.plays++;
    fake.lastEffect = effect;
    fake.busy = true;
    return 0;
}

static int fakePulse(void *ctx, unsigned ms) {
    (void)ctx;
    fake.pulses++;
    fake.lastMs = ms;
    fake.busy = true;
    return 0;
}

static void fakeStop(void *ctx) {
    (void)ctx;
    fake.stops++;
    fake.busy = false;
}

static bool fakeBusy(const void *ctx) {
    (void)ctx;
    return fake.busy;
}

static const haptic_ops_t fakeOps = {fakePlay, fakePulse, fakeStop, fakeBusy};

static haptic_device_desc_t mainDesc(unsigned effects) {
    haptic_device_desc_t d = {"main", effects, &fakeOps, NULL};
    return d;
}

TEST_SETUP(haptic_board) { memset(&fake, 0, sizeof(fake)); }

TEST_TEAR_DOWN(haptic_board) { teardown(); }

TEST(haptic_board, RegistrationRejectsBadDescriptions) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(NULL));
    d.name = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(&d));
    d = mainDesc(HAPTIC_EFFECTS_ALL);
    d.name = "a b";
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(&d));
    d = mainDesc(1U << HAPTIC_EFFECT_COUNT);
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(&d));
    d = mainDesc(HAPTIC_EFFECTS_ALL);
    d.ops = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(&d));
    haptic_ops_t partial = fakeOps;
    partial.stop = NULL;
    d.ops = &partial;
    TEST_ASSERT_EQUAL_INT(-EINVAL, HapticDeviceRegister(&d));
}

TEST(haptic_board, ADuplicateNameIsRefused) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_EQUAL_INT(-EEXIST, HapticDeviceRegister(&d));
}

TEST(haptic_board, ABareNameSelectsTheBoardDeviceAndPlaysThroughIt) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    sendCtl("play alert");
    TEST_ASSERT_EQUAL_UINT(1, fake.plays);
    TEST_ASSERT_EQUAL_UINT(HAPTIC_EFFECT_ALERT, fake.lastEffect);
    TEST_ASSERT_EQUAL_INT(1, busy(&owner));
    sendCtl("pulse 80");
    TEST_ASSERT_EQUAL_UINT(80, fake.lastMs);
}

TEST(haptic_board, EffectsShowOnlyWhatTheDevicePlays) {
    char buf[64];
    haptic_device_desc_t d =
        mainDesc((1U << HAPTIC_EFFECT_CLICK) | (1U << HAPTIC_EFFECT_BUZZ));
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/effects", buf, 64));
    TEST_ASSERT_EQUAL_STRING("click\nbuzz\n", buf);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("play double"));
    TEST_ASSERT_EQUAL_UINT(0, fake.plays);
}

TEST(haptic_board, ABackingErrorReachesTheWriter) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    fake.failPlay = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, ctl("play click"));
}

TEST(haptic_board, AGenericEntryCannotTakeABoardName) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_NULL(VfsHapticInit(NULL, "motors=main:12:onoff"));
}

TEST(haptic_board, ABoardDeviceHasOneWriterAndIsFreedOnDestroy) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    TEST_ASSERT_FALSE(attach(&other, "motors=main"));
    sendCtl("pulse 500");
    detach(&owner);
    TEST_ASSERT_EQUAL_UINT(1, fake.stops);
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
}

TEST(haptic_board, AnObserverReadsWithoutCtlAndDoesNotTakeTheWriter) {
    char names[64];
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&other, "motors=main,observe"));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    TEST_ASSERT_EQUAL_INT(0, listDir(&other, "main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("effects,busy", names);
    TEST_ASSERT_EQUAL_INT(-ENOENT, writeVia(&other, "main/ctl", "stop"));
    sendCtl("pulse 100");
    TEST_ASSERT_EQUAL_INT(1, busy(&other));
}

TEST(haptic_board, AnObserverGoingAwayDoesNotStopTheMotor) {
    haptic_device_desc_t d = mainDesc(HAPTIC_EFFECTS_ALL);
    TEST_ASSERT_EQUAL_INT(0, HapticDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "motors=main"));
    TEST_ASSERT_TRUE(attach(&other, "motors=main,observe"));
    sendCtl("pulse 100");
    detach(&other);
    TEST_ASSERT_EQUAL_UINT(0, fake.stops);
}

TEST_GROUP_RUNNER(haptic_board) {
    RUN_TEST_CASE(haptic_board, RegistrationRejectsBadDescriptions);
    RUN_TEST_CASE(haptic_board, ADuplicateNameIsRefused);
    RUN_TEST_CASE(haptic_board,
                  ABareNameSelectsTheBoardDeviceAndPlaysThroughIt);
    RUN_TEST_CASE(haptic_board, EffectsShowOnlyWhatTheDevicePlays);
    RUN_TEST_CASE(haptic_board, ABackingErrorReachesTheWriter);
    RUN_TEST_CASE(haptic_board, AGenericEntryCannotTakeABoardName);
    RUN_TEST_CASE(haptic_board, ABoardDeviceHasOneWriterAndIsFreedOnDestroy);
    RUN_TEST_CASE(haptic_board,
                  AnObserverReadsWithoutCtlAndDoesNotTakeTheWriter);
    RUN_TEST_CASE(haptic_board, AnObserverGoingAwayDoesNotStopTheMotor);
}
