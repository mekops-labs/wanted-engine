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
#include <vfs-input.h>
#include <vfs-led.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* /dev/led: the per-LED subtree, the grant grammar, fades and triggers. The
 * pin backing is the state-only one, whose fades follow the monotonic clock,
 * so a test steps time with DummyClockAdvance and calls LedTick. */

#define NS_PER_MS 1000000ULL
#define KBD_PWM "leds=kbd:46:pwm"
#define KBD_ACTIVITY "leds=kbd:46:pwm,activity=kbd:main"

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t other;
static input_device_t *mainDev;
static input_device_t *auxDev;

static void registerInputs(void) {
    input_device_desc_t main = {"main", INPUT_TYPE_KEY, "default"};
    input_device_desc_t aux = {"aux", INPUT_TYPE_KEY, "default"};
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&main, &mainDev));
    TEST_ASSERT_EQUAL_INT(0, InputDeviceRegister(&aux, &auxDev));
}

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsLedInit(NULL, options);
    if (p->drv == NULL) {
        VfsDestroy(&p->vfs);
        return false;
    }
    DevFs_Register(p->vfs, "led", p->drv);
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
    LedDevicesReset();
    InputDevicesReset();
}

static void advanceMs(unsigned ms) { DummyClockAdvance(ms * NS_PER_MS); }

static void press(input_device_t *dev) {
    wanted_input_event_t ev = {1, EV_KEY, 30, 1};
    InputDevicePush(dev, &ev);
}

/* `path` is relative to /dev/led. */
static const char *fullPath(const char *path, char *out, size_t outLen) {
    snprintf(out, outLen, "/dev/led%s%s", path[0] != '\0' ? "/" : "", path);
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

static int writeNode(const char *path, const char *payload) {
    char full[64];
    int fd =
        VfsOpen(owner.vfs, fullPath(path, full, sizeof(full)), VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(owner.vfs, fd, payload, strlen(payload));
    VfsClose(owner.vfs, fd);
    return n;
}

/* A node's decimal value, or a negative errno. */
static int readNumber(const char *path) {
    char buf[32];
    int n = readVia(&owner, path, buf, sizeof(buf));
    if (n < 0)
        return n;
    return atoi(buf);
}

static int brightness(void) { return readNumber("kbd/brightness"); }

static void setNode(const char *path, const char *payload) {
    TEST_ASSERT_EQUAL_INT((int)strlen(payload), writeNode(path, payload));
}

/* The names in a directory, joined by ',' into `out`. Returns 0 or -errno. */
static int listDir(const char *path, char *out, size_t outLen) {
    char full[64];
    int fd =
        VfsOpen(owner.vfs, fullPath(path, full, sizeof(full)), VFS_O_RDONLY);
    if (fd < 0)
        return fd;

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int rc = VfsReadDir(owner.vfs, fd, buf, sizeof(buf), &cookie, &used);
    VfsClose(owner.vfs, fd);
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
TEST_GROUP(led_grant);
/***************************************/

TEST_SETUP(led_grant) { registerInputs(); }

TEST_TEAR_DOWN(led_grant) { teardown(); }

TEST(led_grant, MissingLedsClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsLedInit(NULL, ""));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds="));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "led=kbd:46:pwm"));
}

TEST(led_grant, MalformedEntryFailsLaunch) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm:x"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:rgb"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=:46:pwm"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd::pwm"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=k b:46:pwm"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=abcdefghijklmnop:46:pwm"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,,s:21:onoff"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,"));
}

TEST(led_grant, UnknownBoardNameFailsLaunch) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=display"));
}

TEST(led_grant, RepeatedNameFailsLaunch) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,kbd:47:pwm"));
}

TEST(led_grant, RepeatedAddressFailsLaunchAndFreesTheFirst) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=a:46:pwm,b:46:pwm"));
    TEST_ASSERT_TRUE(attach(&owner, "leds=a:46:pwm"));
}

TEST(led_grant, ObserveIsNotImplementedAndFailsLaunch) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,observe"));
}

TEST(led_grant, ActivityClauseNeedsAGrantedLedAndAnInput) {
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,activity=nope:main"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,activity=kbd:nope"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,activity=kbd"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,activity=:main"));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=kbd:46:pwm,activity=kbd:"));
}

TEST(led_grant, ActivityClauseRepeatedForOneLedFailsLaunch) {
    TEST_ASSERT_NULL(
        VfsLedInit(NULL, "leds=kbd:46:pwm,activity=kbd:main,activity=kbd:aux"));
}

TEST(led_grant, EachLedMayNameItsOwnActivityInput) {
    TEST_ASSERT_TRUE(attach(
        &owner, "leds=kbd:46:pwm,s:21:onoff,activity=s:aux,activity=kbd:main"));
}

TEST(led_grant, SecondWriterOfALedFailsLaunchWithBusy) {
    TEST_ASSERT_TRUE(attach(&owner, KBD_PWM));
    TEST_ASSERT_FALSE(attach(&other, KBD_PWM));
    TEST_ASSERT_FALSE(attach(&other, "leds=kbd:47:pwm"));
    TEST_ASSERT_FALSE(attach(&other, "leds=x:46:pwm"));
}

TEST(led_grant, ReleasedLedCanBeGrantedAgain) {
    TEST_ASSERT_TRUE(attach(&owner, KBD_PWM));
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, KBD_PWM));
}

TEST(led_grant, ReadDirListsExactlyTheGrantedLeds) {
    char names[128];
    TEST_ASSERT_TRUE(attach(&owner, "leds=kbd:46:pwm,s:21:onoff"));
    TEST_ASSERT_EQUAL_INT(0, listDir("", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("kbd,s", names);
    TEST_ASSERT_EQUAL_INT(0, listDir("kbd", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING(
        "brightness,max_brightness,trigger,idle_ms,idle_level,ctl", names);
}

TEST(led_grant, UngrantedNamesAndNodesAreAbsent) {
    char buf[16];
    TEST_ASSERT_TRUE(attach(&owner, KBD_PWM));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          readVia(&owner, "other/brightness", buf, 16));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "kbd/nope", buf, 16));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "kbd/observe", buf, 16));
}

TEST_GROUP_RUNNER(led_grant) {
    RUN_TEST_CASE(led_grant, MissingLedsClauseFailsLaunch);
    RUN_TEST_CASE(led_grant, MalformedEntryFailsLaunch);
    RUN_TEST_CASE(led_grant, UnknownBoardNameFailsLaunch);
    RUN_TEST_CASE(led_grant, RepeatedNameFailsLaunch);
    RUN_TEST_CASE(led_grant, RepeatedAddressFailsLaunchAndFreesTheFirst);
    RUN_TEST_CASE(led_grant, ObserveIsNotImplementedAndFailsLaunch);
    RUN_TEST_CASE(led_grant, ActivityClauseNeedsAGrantedLedAndAnInput);
    RUN_TEST_CASE(led_grant, ActivityClauseRepeatedForOneLedFailsLaunch);
    RUN_TEST_CASE(led_grant, EachLedMayNameItsOwnActivityInput);
    RUN_TEST_CASE(led_grant, SecondWriterOfALedFailsLaunchWithBusy);
    RUN_TEST_CASE(led_grant, ReleasedLedCanBeGrantedAgain);
    RUN_TEST_CASE(led_grant, ReadDirListsExactlyTheGrantedLeds);
    RUN_TEST_CASE(led_grant, UngrantedNamesAndNodesAreAbsent);
}

/***************************************/
TEST_GROUP(led_brightness);
/***************************************/

TEST_SETUP(led_brightness) { TEST_ASSERT_TRUE(attach(&owner, KBD_PWM)); }

TEST_TEAR_DOWN(led_brightness) { teardown(); }

TEST(led_brightness, MaxBrightnessIs255ForPwmAndOneForOnOff) {
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, "leds=kbd:46:pwm,s:21:onoff"));
    TEST_ASSERT_EQUAL_INT(255, readNumber("kbd/max_brightness"));
    TEST_ASSERT_EQUAL_INT(1, readNumber("s/max_brightness"));
}

TEST(led_brightness, StartsDarkAndReadsBackWhatWasWritten) {
    TEST_ASSERT_EQUAL_INT(0, brightness());
    setNode("kbd/brightness", "200\n");
    TEST_ASSERT_EQUAL_INT(200, brightness());
    setNode("kbd/brightness", "0");
    TEST_ASSERT_EQUAL_INT(0, brightness());
}

TEST(led_brightness, ValuesOutsideTheRangeOrNotNumbersAreRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/brightness", "256"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/brightness", "-1"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/brightness", "abc"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/brightness", ""));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/brightness", "1 2"));
    TEST_ASSERT_EQUAL_INT(0, brightness());
}

TEST(led_brightness, ReadOnlyAndWriteOnlyNodesRefuse) {
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("kbd/max_brightness", "1"));
    TEST_ASSERT_EQUAL_INT(-EPERM, readVia(&owner, "kbd/ctl", buf, 16));
}

TEST(led_brightness, FadeRampsOverTheRequestedTime) {
    setNode("kbd/ctl", "fade 200 1000\n");
    TEST_ASSERT_EQUAL_INT(0, brightness());
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(100, brightness());
    advanceMs(600);
    TEST_ASSERT_EQUAL_INT(200, brightness());
}

TEST(led_brightness, FadeFromTheCurrentLevelDownToZero) {
    setNode("kbd/brightness", "100");
    setNode("kbd/ctl", "fade 0 1000");
    advanceMs(250);
    TEST_ASSERT_EQUAL_INT(75, brightness());
}

TEST(led_brightness, BrightnessWriteCancelsARunningFade) {
    setNode("kbd/ctl", "fade 200 1000");
    advanceMs(500);
    setNode("kbd/brightness", "30");
    advanceMs(1000);
    TEST_ASSERT_EQUAL_INT(30, brightness());
}

TEST(led_brightness, FadeOfZeroDurationIsInstant) {
    setNode("kbd/ctl", "fade 50 0");
    TEST_ASSERT_EQUAL_INT(50, brightness());
}

TEST(led_brightness, BadFadeLinesAreRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "fade 300 100"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "fade 10"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "fade a b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "fade 10 100 5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "fade 10 -5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", "blink"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/ctl", ""));
}

TEST(led_brightness, OnOffLedSwitchesAtTheEndOfAFade) {
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, "leds=s:21:onoff"));
    setNode("s/ctl", "fade 1 1000");
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(0, readNumber("s/brightness"));
    advanceMs(600);
    TEST_ASSERT_EQUAL_INT(1, readNumber("s/brightness"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("s/brightness", "2"));
}

TEST_GROUP_RUNNER(led_brightness) {
    RUN_TEST_CASE(led_brightness, MaxBrightnessIs255ForPwmAndOneForOnOff);
    RUN_TEST_CASE(led_brightness, StartsDarkAndReadsBackWhatWasWritten);
    RUN_TEST_CASE(led_brightness, ValuesOutsideTheRangeOrNotNumbersAreRejected);
    RUN_TEST_CASE(led_brightness, ReadOnlyAndWriteOnlyNodesRefuse);
    RUN_TEST_CASE(led_brightness, FadeRampsOverTheRequestedTime);
    RUN_TEST_CASE(led_brightness, FadeFromTheCurrentLevelDownToZero);
    RUN_TEST_CASE(led_brightness, BrightnessWriteCancelsARunningFade);
    RUN_TEST_CASE(led_brightness, FadeOfZeroDurationIsInstant);
    RUN_TEST_CASE(led_brightness, BadFadeLinesAreRejected);
    RUN_TEST_CASE(led_brightness, OnOffLedSwitchesAtTheEndOfAFade);
}

/***************************************/
TEST_GROUP(led_activity);
/***************************************/

TEST_SETUP(led_activity) {
    registerInputs();
    TEST_ASSERT_TRUE(attach(&owner, KBD_ACTIVITY));
}

TEST_TEAR_DOWN(led_activity) { teardown(); }

/* Brightness 200 held, idle after 2 s, dimmed to 20. */
static void armActivity(void) {
    setNode("kbd/brightness", "200");
    setNode("kbd/idle_ms", "2000");
    setNode("kbd/idle_level", "20");
    setNode("kbd/trigger", "activity");
}

TEST(led_activity, TriggerDefaultsToNoneAndRejectsUnknownNames) {
    char buf[16];
    TEST_ASSERT_EQUAL_INT(5, readVia(&owner, "kbd/trigger", buf, 16));
    TEST_ASSERT_EQUAL_STRING("none\n", buf);
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/trigger", "blink"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/trigger", ""));
    setNode("kbd/trigger", "activity\n");
    TEST_ASSERT_EQUAL_INT(9, readVia(&owner, "kbd/trigger", buf, 16));
    TEST_ASSERT_EQUAL_STRING("activity\n", buf);
}

TEST(led_activity, IdleSettingsHaveDefaultsAndLimits) {
    TEST_ASSERT_EQUAL_INT(10000, readNumber("kbd/idle_ms"));
    TEST_ASSERT_EQUAL_INT(0, readNumber("kbd/idle_level"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/idle_level", "256"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/idle_ms", "-1"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/idle_ms", "x"));
    setNode("kbd/idle_ms", "0");
    TEST_ASSERT_EQUAL_INT(0, readNumber("kbd/idle_ms"));
}

TEST(led_activity, WithoutAnInputDeviceTheTriggerIsRefused) {
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, KBD_PWM));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("kbd/trigger", "activity"));
}

TEST(led_activity, DimsToTheIdleLevelThenReturnsOnInput) {
    armActivity();
    advanceMs(1900);
    LedTick();
    TEST_ASSERT_EQUAL_INT(200, brightness());

    advanceMs(200);
    LedTick();
    TEST_ASSERT_EQUAL_INT(200, brightness());
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(110, brightness());
    advanceMs(600);
    TEST_ASSERT_EQUAL_INT(20, brightness());

    /* The dummy clock moves 1 ms per read, which is 1.8 levels of a 100 ms
     * fade, so the fast fade is checked within a few levels. */
    press(mainDev);
    TEST_ASSERT_INT_WITHIN(4, 20, brightness());
    advanceMs(50);
    TEST_ASSERT_INT_WITHIN(8, 110, brightness());
    advanceMs(60);
    TEST_ASSERT_EQUAL_INT(200, brightness());
}

TEST(led_activity, InputRestartsTheIdleTimer) {
    armActivity();
    advanceMs(1500);
    press(mainDev);
    advanceMs(1500);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(200, brightness());
}

TEST(led_activity, InputOnAnotherDeviceDoesNotCount) {
    armActivity();
    advanceMs(1500);
    press(auxDev);
    advanceMs(600);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(20, brightness());
}

TEST(led_activity, IdleMsOfZeroNeverFades) {
    armActivity();
    setNode("kbd/idle_ms", "0");
    advanceMs(60000);
    LedTick();
    advanceMs(2000);
    TEST_ASSERT_EQUAL_INT(200, brightness());
}

TEST(led_activity, TheLevelLastWrittenIsTheOneReturnedTo) {
    armActivity();
    setNode("kbd/brightness", "100");
    advanceMs(2100);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(20, brightness());
    press(mainDev);
    advanceMs(110);
    TEST_ASSERT_EQUAL_INT(100, brightness());
}

TEST(led_activity, AFadeWrittenWhileActiveBecomesTheLevelHeld) {
    armActivity();
    setNode("kbd/ctl", "fade 60 100");
    advanceMs(2100);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(20, brightness());
    press(mainDev);
    advanceMs(110);
    TEST_ASSERT_EQUAL_INT(60, brightness());
}

TEST(led_activity, AWriteWhileDimmedLightsTheLedAndRestartsTheTimer) {
    armActivity();
    advanceMs(2100);
    LedTick();
    advanceMs(1100);
    setNode("kbd/brightness", "80");
    TEST_ASSERT_EQUAL_INT(80, brightness());
    advanceMs(1500);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(80, brightness());

    advanceMs(600);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(20, brightness());
}

TEST(led_activity, IdleLevelAboveTheHeldLevelNeverBrightens) {
    armActivity();
    setNode("kbd/brightness", "5");
    advanceMs(2100);
    LedTick();
    advanceMs(1100);
    TEST_ASSERT_EQUAL_INT(5, brightness());
}

TEST_GROUP_RUNNER(led_activity) {
    RUN_TEST_CASE(led_activity, TriggerDefaultsToNoneAndRejectsUnknownNames);
    RUN_TEST_CASE(led_activity, IdleSettingsHaveDefaultsAndLimits);
    RUN_TEST_CASE(led_activity, WithoutAnInputDeviceTheTriggerIsRefused);
    RUN_TEST_CASE(led_activity, DimsToTheIdleLevelThenReturnsOnInput);
    RUN_TEST_CASE(led_activity, InputRestartsTheIdleTimer);
    RUN_TEST_CASE(led_activity, InputOnAnotherDeviceDoesNotCount);
    RUN_TEST_CASE(led_activity, IdleMsOfZeroNeverFades);
    RUN_TEST_CASE(led_activity, TheLevelLastWrittenIsTheOneReturnedTo);
    RUN_TEST_CASE(led_activity, AFadeWrittenWhileActiveBecomesTheLevelHeld);
    RUN_TEST_CASE(led_activity,
                  AWriteWhileDimmedLightsTheLedAndRestartsTheTimer);
    RUN_TEST_CASE(led_activity, IdleLevelAboveTheHeldLevelNeverBrightens);
}

/***************************************/
TEST_GROUP(led_heartbeat);
/***************************************/

TEST_SETUP(led_heartbeat) {
    TEST_ASSERT_TRUE(attach(&owner, "leds=kbd:46:pwm,s:21:onoff"));
}

TEST_TEAR_DOWN(led_heartbeat) { teardown(); }

TEST(led_heartbeat, ABeatIsABreathEveryFiveSeconds) {
    setNode("kbd/trigger", "heartbeat");
    LedTick();
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(255, brightness());

    advanceMs(500);
    LedTick();
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(0, brightness());

    for (int i = 0; i < 3; i++) {
        advanceMs(1000);
        LedTick();
    }
    TEST_ASSERT_EQUAL_INT(0, brightness());

    advanceMs(1000);
    LedTick();
    advanceMs(500);
    TEST_ASSERT_EQUAL_INT(255, brightness());
}

TEST(led_heartbeat, BeatsStopWhenTheTicksStop) {
    setNode("kbd/trigger", "heartbeat");
    LedTick();
    advanceMs(60000);
    TEST_ASSERT_EQUAL_INT(255, brightness());
}

TEST(led_heartbeat, AnOnOffLedBlinksOnAndOff) {
    setNode("s/trigger", "heartbeat");
    LedTick();
    TEST_ASSERT_EQUAL_INT(1, readNumber("s/brightness"));
    advanceMs(1000);
    LedTick();
    TEST_ASSERT_EQUAL_INT(0, readNumber("s/brightness"));
}

TEST(led_heartbeat, TheTriggerOwnsTheLedUntilNoneIsWritten) {
    setNode("kbd/trigger", "heartbeat");
    TEST_ASSERT_EQUAL_INT(-EBUSY, writeNode("kbd/brightness", "10"));
    TEST_ASSERT_EQUAL_INT(-EBUSY, writeNode("kbd/ctl", "fade 10 100"));
    LedTick();
    advanceMs(600);
    setNode("kbd/trigger", "none");
    TEST_ASSERT_EQUAL_INT(0, brightness());
    setNode("kbd/brightness", "10");
    TEST_ASSERT_EQUAL_INT(10, brightness());
}

TEST_GROUP_RUNNER(led_heartbeat) {
    RUN_TEST_CASE(led_heartbeat, ABeatIsABreathEveryFiveSeconds);
    RUN_TEST_CASE(led_heartbeat, BeatsStopWhenTheTicksStop);
    RUN_TEST_CASE(led_heartbeat, AnOnOffLedBlinksOnAndOff);
    RUN_TEST_CASE(led_heartbeat, TheTriggerOwnsTheLedUntilNoneIsWritten);
}

/***************************************/
TEST_GROUP(led_board);
/***************************************/

typedef struct fake_t {
    unsigned level;
    unsigned lastFadeLevel;
    unsigned lastFadeMs;
    int fades;
} fake_t;

static fake_t fake;

static int fakeSet(void *ctx, unsigned level) {
    fake_t *f = ctx;
    f->level = level;
    return 0;
}

static int fakeFade(void *ctx, unsigned level, unsigned ms) {
    fake_t *f = ctx;
    f->lastFadeLevel = level;
    f->lastFadeMs = ms;
    f->fades++;
    f->level = level;
    return 0;
}

static unsigned fakeGet(const void *ctx) {
    return ((const fake_t *)ctx)->level;
}

static const led_ops_t fakeOps = {fakeSet, fakeFade, fakeGet};

static led_device_desc_t displayDesc(void) {
    led_device_desc_t d = {"display", 16, &fakeOps, &fake, false};
    return d;
}

TEST_SETUP(led_board) {
    memset(&fake, 0, sizeof(fake));
    registerInputs();
}

TEST_TEAR_DOWN(led_board) { teardown(); }

TEST(led_board, RegistrationRejectsBadDescriptions) {
    led_device_desc_t d = displayDesc();
    d.name = "bad name";
    TEST_ASSERT_EQUAL_INT(-EINVAL, LedDeviceRegister(&d));
    d = displayDesc();
    d.max = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, LedDeviceRegister(&d));
    d = displayDesc();
    d.ops = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, LedDeviceRegister(&d));
    TEST_ASSERT_EQUAL_INT(-EINVAL, LedDeviceRegister(NULL));
}

TEST(led_board, ADuplicateNameIsRefused) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_EQUAL_INT(-EEXIST, LedDeviceRegister(&d));
}

TEST(led_board, ABareNameSelectsTheBoardDevice) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "leds=display"));
    TEST_ASSERT_EQUAL_INT(16, readNumber("display/max_brightness"));
    setNode("display/brightness", "9");
    TEST_ASSERT_EQUAL_UINT(9, fake.level);
    TEST_ASSERT_EQUAL_INT(9, readNumber("display/brightness"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("display/brightness", "17"));
    setNode("display/ctl", "fade 4 300");
    TEST_ASSERT_EQUAL_UINT(4, fake.lastFadeLevel);
    TEST_ASSERT_EQUAL_UINT(300, fake.lastFadeMs);
}

TEST(led_board, AFadeOfZeroDurationIsASet) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "leds=display"));
    setNode("display/ctl", "fade 4 0");
    TEST_ASSERT_EQUAL_UINT(4, fake.level);
    TEST_ASSERT_EQUAL_INT(0, fake.fades);
}

TEST(led_board, AGenericEntryCannotTakeABoardName) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_NULL(VfsLedInit(NULL, "leds=display:46:pwm"));
}

TEST(led_board, ABoardDeviceHasOneWriterAndIsFreedOnDestroy) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "leds=display"));
    TEST_ASSERT_FALSE(attach(&other, "leds=display"));
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, "leds=display"));
}

TEST(led_board, TheActivityTriggerDrivesTheBoardFades) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "leds=display,activity=display:main"));
    setNode("display/brightness", "12");
    setNode("display/idle_ms", "1000");
    setNode("display/idle_level", "1");
    setNode("display/trigger", "activity");
    advanceMs(1100);
    LedTick();
    TEST_ASSERT_EQUAL_UINT(1, fake.lastFadeLevel);
    TEST_ASSERT_EQUAL_UINT(1000, fake.lastFadeMs);
    press(mainDev);
    TEST_ASSERT_EQUAL_UINT(12, fake.lastFadeLevel);
    TEST_ASSERT_EQUAL_UINT(100, fake.lastFadeMs);
}

TEST(led_board, TheActivityLinkEndsWithTheWriter) {
    led_device_desc_t d = displayDesc();
    TEST_ASSERT_EQUAL_INT(0, LedDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, "leds=display,activity=display:main"));
    setNode("display/trigger", "activity");
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, "leds=display"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeNode("display/trigger", "activity"));
}

TEST(led_board, TicksAndInputWithNoLedsAreHarmless) {
    LedTick();
    press(mainDev);
    TEST_PASS();
}

TEST_GROUP_RUNNER(led_board) {
    RUN_TEST_CASE(led_board, RegistrationRejectsBadDescriptions);
    RUN_TEST_CASE(led_board, ADuplicateNameIsRefused);
    RUN_TEST_CASE(led_board, ABareNameSelectsTheBoardDevice);
    RUN_TEST_CASE(led_board, AFadeOfZeroDurationIsASet);
    RUN_TEST_CASE(led_board, AGenericEntryCannotTakeABoardName);
    RUN_TEST_CASE(led_board, ABoardDeviceHasOneWriterAndIsFreedOnDestroy);
    RUN_TEST_CASE(led_board, TheActivityTriggerDrivesTheBoardFades);
    RUN_TEST_CASE(led_board, TheActivityLinkEndsWithTheWriter);
    RUN_TEST_CASE(led_board, TicksAndInputWithNoLedsAreHarmless);
}
