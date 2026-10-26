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
#include <vfs-power.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* /dev/power: the grant grammar, the policy, the ladder, the two-phase
 * transition, the board's safety rules and the clients. The dummy platform
 * never sleeps: a sleep returns at once with the cause a test scripted. */

#define NS_PER_S 1000000000ULL
#define CONTROL "devices=main,control"
#define READONLY "devices=main"

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t viewer;

/* What the fake board and the clients saw, in order. */
static char trace[64];

typedef struct fake_t {
    unsigned suspends;
    unsigned sleeps;
    unsigned poweroffs;
    unsigned resumes;
    power_arm_t arm;
    bool ext;
    int failSuspend;
    const char *word;
    unsigned railsCalls;
} fake_t;

static fake_t fake;

static void note(char c) {
    size_t n = strlen(trace);
    if (n + 1 < sizeof(trace)) {
        trace[n] = c;
        trace[n + 1] = '\0';
    }
}

static int fakeSuspend(void *ctx, const power_arm_t *arm) {
    (void)ctx;
    fake.suspends++;
    fake.arm = *arm;
    note('s');
    return fake.failSuspend;
}

static const char *fakeResume(void *ctx, power_cause_t cause) {
    (void)ctx;
    (void)cause;
    fake.resumes++;
    note('r');
    return fake.word;
}

static int fakeSleep(void *ctx, const power_arm_t *arm) {
    (void)ctx;
    fake.sleeps++;
    fake.arm = *arm;
    note('d');
    return 0;
}

static int fakePoweroff(void *ctx) {
    (void)ctx;
    fake.poweroffs++;
    note('o');
    return 0;
}

static bool fakeExt(void *ctx) {
    (void)ctx;
    return fake.ext;
}

static size_t fakeRails(void *ctx, char *buf, size_t len) {
    (void)ctx;
    fake.railsCalls++;
    return (size_t)snprintf(buf, len, "lora on holders=lora:main\n");
}

static const power_ops_t fakeOps = {fakeSuspend,  fakeResume, fakeSleep,
                                    fakePoweroff, fakeExt,    fakeRails};

static const power_wake_desc_t fakeWakes[] = {
    {"key", POWER_BIT(POWER_STANDBY) | POWER_BIT(POWER_SUSPEND) |
                POWER_BIT(POWER_SLEEP)},
    {"touch", POWER_BIT(POWER_STANDBY)},
};

static power_device_desc_t mainDesc(void) {
    power_device_desc_t d = {"main",
                             POWER_BIT(POWER_STANDBY) |
                                 POWER_BIT(POWER_SUSPEND) |
                                 POWER_BIT(POWER_SLEEP),
                             fakeWakes,
                             2,
                             &fakeOps,
                             NULL,
                             false};
    return d;
}

static int clientA(void *ctx) {
    (void)ctx;
    note('A');
    return 0;
}

static int clientB(void *ctx) {
    (void)ctx;
    note('B');
    return 0;
}

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsPowerInit(NULL, options);
    if (p->drv == NULL) {
        VfsDestroy(&p->vfs);
        return false;
    }
    DevFs_Register(p->vfs, "power", p->drv);
    return true;
}

static void detach(peer_t *p) {
    if (p->drv != NULL)
        VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static void teardown(void) {
    detach(&viewer);
    detach(&owner);
    PowerDevicesReset();
    DummyPowerReset();
}

static void setup(void) {
    memset(&fake, 0, sizeof(fake));
    trace[0] = '\0';
    DummyPowerReset();
}

static void startDevice(void) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
}

/* `path` is relative to /dev/power. */
static const char *fullPath(const char *path, char *out, size_t outLen) {
    snprintf(out, outLen, "/dev/power%s%s", path[0] != '\0' ? "/" : "", path);
    return out;
}

static int readVia(peer_t *p, const char *path, char *buf, size_t bufLen) {
    char full[64];
    int fd = VfsOpen(p->vfs, fullPath(path, full, sizeof(full)),
                     VFS_O_RDONLY | VFS_O_NONBLOCK);
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

static int policy(const char *text) {
    return writeVia(&owner, "main/policy", text);
}

static void setPolicy(const char *text) {
    TEST_ASSERT_EQUAL_INT((int)strlen(text), policy(text));
}

static int ctl(const char *line) { return writeVia(&owner, "main/ctl", line); }

static void sendCtl(const char *line) {
    TEST_ASSERT_EQUAL_INT((int)strlen(line), ctl(line));
}

/* Everything queued on event, or "" when nothing waits. */
static const char *events(void) {
    static char buf[128];
    int n = readVia(&owner, "main/event", buf, sizeof(buf));
    if (n < 0)
        buf[0] = '\0';
    return buf;
}

static const char *modeName(void) {
    static char buf[32];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/mode", buf, sizeof(buf)));
    char *sp = strchr(buf, ' ');
    if (sp != NULL)
        *sp = '\0';
    return buf;
}

/* Advance `s` seconds, ticking each second. */
static void run(unsigned s) {
    for (unsigned i = 0; i < s; i++) {
        DummyClockAdvance(NS_PER_S);
        PowerTick();
    }
}

static void runMs(unsigned ms) {
    DummyClockAdvance((uint64_t)ms * 1000000ULL);
    PowerTick();
}

/***************************************/
TEST_GROUP(power_grant);
/***************************************/

TEST_SETUP(power_grant) { setup(); }

TEST_TEAR_DOWN(power_grant) { teardown(); }

TEST(power_grant, MissingDevicesClauseFailsLaunch) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, ""));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "devices="));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "device=main"));
}

TEST(power_grant, UnknownDeviceOrClauseFailsLaunch) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "devices=aux"));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "devices=main,ctl"));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "devices=main,control,control"));
    TEST_ASSERT_NULL(VfsPowerInit(NULL, "devices=main,aux"));
}

TEST(power_grant, ASecondControlGrantFailsAndReadOnlyGrantsDoNot) {
    startDevice();
    TEST_ASSERT_FALSE(attach(&viewer, CONTROL));
    TEST_ASSERT_TRUE(attach(&viewer, READONLY));
    peer_t third = {0};
    third.vfs = VfsInit();
    third.drv = VfsPowerInit(NULL, READONLY);
    TEST_ASSERT_NOT_NULL(third.drv);
    VfsDestroy(&third.vfs);
}

TEST(power_grant, TheControlGrantFreesWithItsWapp) {
    startDevice();
    detach(&owner);
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
}

TEST(power_grant, ReaddirShowsPolicyAndCtlOnlyWithControl) {
    char buf[512];
    startDevice();
    TEST_ASSERT_TRUE(attach(&viewer, READONLY));
    int fd = VfsOpen(owner.vfs, "/dev/power/main", VFS_O_RDONLY);
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(owner.vfs, fd, buf, sizeof(buf), &cookie, &used));
    VfsClose(owner.vfs, fd);
    size_t full = used;

    fd = VfsOpen(viewer.vfs, "/dev/power/main", VFS_O_RDONLY);
    cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(viewer.vfs, fd, buf, sizeof(buf), &cookie, &used));
    VfsClose(viewer.vfs, fd);
    TEST_ASSERT_LESS_THAN(full, used);
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&viewer, "main/policy", buf, 64));
    TEST_ASSERT_EQUAL_INT(-ENOENT, writeVia(&viewer, "main/ctl", "activity"));
}

TEST(power_grant, AnUngrantedNameIsAbsent) {
    char buf[32];
    startDevice();
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "aux/mode", buf, 32));
    TEST_ASSERT_EQUAL_INT(-ENOENT, readVia(&owner, "main/nope", buf, 32));
}

TEST_GROUP_RUNNER(power_grant) {
    RUN_TEST_CASE(power_grant, MissingDevicesClauseFailsLaunch);
    RUN_TEST_CASE(power_grant, UnknownDeviceOrClauseFailsLaunch);
    RUN_TEST_CASE(power_grant, ASecondControlGrantFailsAndReadOnlyGrantsDoNot);
    RUN_TEST_CASE(power_grant, TheControlGrantFreesWithItsWapp);
    RUN_TEST_CASE(power_grant, ReaddirShowsPolicyAndCtlOnlyWithControl);
    RUN_TEST_CASE(power_grant, AnUngrantedNameIsAbsent);
}

/***************************************/
TEST_GROUP(power_policy);
/***************************************/

TEST_SETUP(power_policy) {
    setup();
    startDevice();
}

TEST_TEAR_DOWN(power_policy) { teardown(); }

TEST(power_policy, ThePoliciesOfTheThreeDevicesParse) {
    setPolicy("standby idle=60\n"
              "sleep after=1800 battery wake=key alarm=1800 resume=active\n"
              "off idle=86400 battery");
    setPolicy("suspend wake=key\noff after=3600");
    setPolicy("standby idle=300");
}

TEST(power_policy, AReadReturnsThePolicyInForce) {
    char buf[128];
    setPolicy("suspend wake=key\noff after=3600\n");
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/policy", buf, 128));
    TEST_ASSERT_EQUAL_STRING("suspend wake=key\noff after=3600\n", buf);
}

TEST(power_policy, ANewlineAloneClearsThePolicy) {
    char buf[16];
    setPolicy("standby idle=60");
    setPolicy("\n");
    TEST_ASSERT_EQUAL_INT(0, readVia(&owner, "main/policy", buf, 16));
}

TEST(power_policy, ABadWriteLeavesThePreviousPolicy) {
    char buf[128];
    setPolicy("standby idle=60");
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=61\nnonsense"));
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/policy", buf, 128));
    TEST_ASSERT_EQUAL_STRING("standby idle=60\n", buf);
}

TEST(power_policy, ModesOutOfOrderOrRepeatedOrUnknownAreRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("off after=5\nsuspend wake=key"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=5\nstandby idle=6"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("active idle=5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("doze idle=5"));
}

TEST(power_policy, AModeTheBoardDoesNotListIsRejected) {
    teardown();
    setup();
    power_device_desc_t d = mainDesc();
    d.modes = POWER_BIT(POWER_SUSPEND);
    d.wakes = fakeWakes;
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("sleep after=5"));
    setPolicy("suspend wake=key\noff after=5");
}

TEST(power_policy, TermsAreValidated) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=0"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=x"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=-5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby idle=5 idle=6"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby bogus=5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby battery battery"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby alarm=5"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby resume=suspend"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby wake="));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("standby after=1234567890"));
}

TEST(power_policy, AWakeSourceTheBoardCannotArmInThatModeIsRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("suspend wake=touch"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("suspend wake=mouse"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy("suspend wake=key,mouse"));
    setPolicy("standby wake=key,touch");
}

TEST(power_policy, AnOversizeWriteIsRejected) {
    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy(big));
    TEST_ASSERT_EQUAL_INT(-EINVAL, policy(""));
}

TEST_GROUP_RUNNER(power_policy) {
    RUN_TEST_CASE(power_policy, ThePoliciesOfTheThreeDevicesParse);
    RUN_TEST_CASE(power_policy, AReadReturnsThePolicyInForce);
    RUN_TEST_CASE(power_policy, ANewlineAloneClearsThePolicy);
    RUN_TEST_CASE(power_policy, ABadWriteLeavesThePreviousPolicy);
    RUN_TEST_CASE(power_policy, ModesOutOfOrderOrRepeatedOrUnknownAreRejected);
    RUN_TEST_CASE(power_policy, AModeTheBoardDoesNotListIsRejected);
    RUN_TEST_CASE(power_policy, TermsAreValidated);
    RUN_TEST_CASE(power_policy,
                  AWakeSourceTheBoardCannotArmInThatModeIsRejected);
    RUN_TEST_CASE(power_policy, AnOversizeWriteIsRejected);
}

/***************************************/
TEST_GROUP(power_ladder);
/***************************************/

TEST_SETUP(power_ladder) {
    setup();
    startDevice();
}

TEST_TEAR_DOWN(power_ladder) { teardown(); }

TEST(power_ladder, WithoutAPolicyTheDeviceStaysActive) {
    run(100);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    TEST_ASSERT_EQUAL_STRING("", events());
}

TEST(power_ladder, IdleQueuesEnterThenAReadyEntersTheMode) {
    setPolicy("standby idle=60");
    run(30);
    TEST_ASSERT_EQUAL_STRING("", events());
    run(31);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
}

TEST(power_ladder, ATransitionWaitsForReadyUntilTheGrace) {
    setPolicy("standby idle=5 grace=4");
    run(6);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
    run(2);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    run(2);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
}

TEST(power_ladder, TheDefaultGraceIsTenSeconds) {
    setPolicy("standby idle=5");
    run(6);
    (void)events();
    run(8);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    run(3);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
}

TEST(power_ladder, ActivityRestartsTheIdleCount) {
    setPolicy("standby idle=60");
    run(50);
    sendCtl("activity");
    run(50);
    TEST_ASSERT_EQUAL_STRING("", events());
    run(15);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
}

TEST(power_ladder, ActivityInStandbyReturnsToActive) {
    setPolicy("standby idle=5");
    run(6);
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
    (void)events();
    sendCtl("activity");
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    TEST_ASSERT_EQUAL_STRING("exit standby activity\n", events());
}

TEST(power_ladder, AModeRequestRunsTheSameTwoPhaseTransition) {
    sendCtl("mode standby");
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
    sendCtl("mode active");
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    TEST_ASSERT_EQUAL_STRING("exit standby request\n", events());
}

TEST(power_ladder, AReadyWithNothingPendingIsIgnored) {
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    sendCtl("mode standby");
    runMs(150);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
}

TEST(power_ladder, ABadCtlLineIsRejected) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("mode"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("mode doze"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("alarm x"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("alarm"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("wake"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("activity now"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl("ready now"));
}

TEST(power_ladder, AfterCountsFromTheMomentOfThePreviousMode) {
    setPolicy("standby idle=5\nsuspend after=20 wake=key");
    run(6);
    sendCtl("ready");
    runMs(150);
    (void)events();
    run(15);
    TEST_ASSERT_EQUAL_STRING("", events());
    run(7);
    TEST_ASSERT_EQUAL_STRING("enter suspend\n", events());
}

TEST(power_ladder, TheBatteryTermBlocksTimersOnExternalPowerOnly) {
    setPolicy("standby idle=5 battery");
    fake.ext = true;
    run(30);
    TEST_ASSERT_EQUAL_STRING("", events());
    fake.ext = false;
    run(1);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
}

TEST(power_ladder, ABatteryTermDoesNotBlockAManualRequest) {
    setPolicy("standby idle=5 battery");
    fake.ext = true;
    sendCtl("mode standby");
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
}

TEST(power_ladder, TheDeadlinesSurviveARewriteOfTheSamePolicy) {
    setPolicy("standby idle=60");
    run(50);
    setPolicy("standby idle=60");
    run(11);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
}

TEST(power_ladder, TheLadderRunsWithNoControlWapp) {
    setPolicy("standby idle=5");
    detach(&owner);
    run(6);
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
    run(11);
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
}

TEST_GROUP_RUNNER(power_ladder) {
    RUN_TEST_CASE(power_ladder, WithoutAPolicyTheDeviceStaysActive);
    RUN_TEST_CASE(power_ladder, IdleQueuesEnterThenAReadyEntersTheMode);
    RUN_TEST_CASE(power_ladder, ATransitionWaitsForReadyUntilTheGrace);
    RUN_TEST_CASE(power_ladder, TheDefaultGraceIsTenSeconds);
    RUN_TEST_CASE(power_ladder, ActivityRestartsTheIdleCount);
    RUN_TEST_CASE(power_ladder, ActivityInStandbyReturnsToActive);
    RUN_TEST_CASE(power_ladder, AModeRequestRunsTheSameTwoPhaseTransition);
    RUN_TEST_CASE(power_ladder, AReadyWithNothingPendingIsIgnored);
    RUN_TEST_CASE(power_ladder, ABadCtlLineIsRejected);
    RUN_TEST_CASE(power_ladder, AfterCountsFromTheMomentOfThePreviousMode);
    RUN_TEST_CASE(power_ladder, TheBatteryTermBlocksTimersOnExternalPowerOnly);
    RUN_TEST_CASE(power_ladder, ABatteryTermDoesNotBlockAManualRequest);
    RUN_TEST_CASE(power_ladder, TheDeadlinesSurviveARewriteOfTheSamePolicy);
    RUN_TEST_CASE(power_ladder, TheLadderRunsWithNoControlWapp);
}

/***************************************/
TEST_GROUP(power_suspend);
/***************************************/

TEST_SETUP(power_suspend) {
    setup();
    startDevice();
    power_client_t a = {"a", clientA, clientA, NULL};
    power_client_t b = {"b", clientB, clientB, NULL};
    TEST_ASSERT_EQUAL_INT(0, PowerClientRegister(&a));
    TEST_ASSERT_EQUAL_INT(0, PowerClientRegister(&b));
}

TEST_TEAR_DOWN(power_suspend) { teardown(); }

/* Suspend through a manual request and its ready. */
static void suspendNow(void) {
    sendCtl("mode suspend");
    sendCtl("ready");
    runMs(150);
}

TEST(power_suspend,
     ClientsSuspendInReverseOrderThenTheBoardAndResumeBackwards) {
    setPolicy("suspend wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("BAsrAB", trace);
    TEST_ASSERT_EQUAL_UINT(1, DummyPowerLightSleeps());
    TEST_ASSERT_EQUAL_UINT(0, DummyPowerDeepSleeps());
}

TEST(power_suspend, ThePolicysWakeSourcesReachTheBoardHook) {
    setPolicy("suspend wake=key");
    suspendNow();
    TEST_ASSERT_EQUAL_UINT(1, fake.arm.wakeMask);
    TEST_ASSERT_FALSE(fake.arm.powerKeyOnly);
}

TEST(power_suspend, AKeyWakeReturnsToActiveAndQueuesAnExit) {
    setPolicy("suspend wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    (void)events();
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    TEST_ASSERT_EQUAL_STRING("enter suspend\nexit suspend key\n", events());
    char buf[16];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/wake", buf, 16));
    TEST_ASSERT_EQUAL_STRING("key\n", buf);
}

TEST(power_suspend, TheResumeTermChoosesTheModeAKeyWakeEnters) {
    setPolicy("standby idle=900\nsuspend wake=key resume=standby");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("standby", modeName());
}

TEST(power_suspend, TheBoardNamesTheWakeWord) {
    setPolicy("suspend wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    fake.word = "touch";
    suspendNow();
    char buf[16];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/wake", buf, 16));
    TEST_ASSERT_EQUAL_STRING("touch\n", buf);
}

TEST(power_suspend, TheNextLadderDeadlineArmsAsTheTimer) {
    setPolicy("suspend wake=key\noff after=3600");
    suspendNow();
    TEST_ASSERT_EQUAL_UINT64(3600ULL * 1000000ULL, fake.arm.timerUs);
    TEST_ASSERT_EQUAL_UINT64(3600ULL * 1000000ULL, DummyPowerLastTimerUs());
}

TEST(power_suspend, WithNoLaterModeThereIsNoTimer) {
    setPolicy("suspend wake=key");
    suspendNow();
    TEST_ASSERT_EQUAL_UINT64(0, fake.arm.timerUs);
}

TEST(power_suspend, ATimerWakeAtTheLadderDeadlineGoesOnToTheNextMode) {
    setPolicy("suspend wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    DummyClockAdvance(3600ULL * NS_PER_S);
    (void)events();
    PowerTick();
    TEST_ASSERT_EQUAL_STRING("enter off\n", events());
    TEST_ASSERT_EQUAL_UINT(0, fake.poweroffs);
    run(11);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

/* The board and the clients stay down from the wake to the power-off. */
TEST(power_suspend, ALadderWakeKeepsTheHardwareDownIntoTheNextMode) {
    setPolicy("suspend wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    DummyClockAdvance(3600ULL * NS_PER_S);
    PowerTick();
    run(11);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
    TEST_ASSERT_EQUAL_STRING("BAsodrAB", trace);
}

TEST(power_suspend, ALadderWakeIntoTheUsbFallbackSuspendsNothingAgain) {
    setPolicy("suspend wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    fake.ext = true;
    DummyClockAdvance(3600ULL * NS_PER_S);
    PowerTick();
    run(11);
    TEST_ASSERT_EQUAL_UINT(0, fake.poweroffs);
    TEST_ASSERT_EQUAL_STRING("BAsdrAB", trace);
}

TEST(power_suspend, ALadderWakeWithNothingDueBringsTheHardwareBack) {
    setPolicy("suspend wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    runMs(500);
    TEST_ASSERT_EQUAL_STRING("BAs", trace);
    run(2);
    TEST_ASSERT_EQUAL_STRING("BAsrAB", trace);
    TEST_ASSERT_EQUAL_STRING("suspend", modeName());
}

TEST(power_suspend, AModeRequestAfterALadderWakeBringsTheHardwareBack) {
    setPolicy("suspend wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    sendCtl("mode active");
    TEST_ASSERT_EQUAL_STRING("BAsrAB", trace);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
}

TEST(power_suspend, AnAlarmWakeAsksTheWappToPrepareAndSleepsAgain) {
    setPolicy("suspend wake=key alarm=60");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("BAsrAB", trace);
    TEST_ASSERT_EQUAL_UINT64(60ULL * 1000000ULL, fake.arm.timerUs);
    TEST_ASSERT_EQUAL_STRING(
        "enter suspend\nexit suspend alarm\nenter suspend\n", events());
    char buf[16];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/wake", buf, 16));
    TEST_ASSERT_EQUAL_STRING("alarm\n", buf);
    TEST_ASSERT_EQUAL_UINT(1, DummyPowerLightSleeps());
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_UINT(2, DummyPowerLightSleeps());
    TEST_ASSERT_EQUAL_STRING("suspend", modeName());
}

TEST(power_suspend, TheAlarmCyclesDoNotResetTheLadderClock) {
    setPolicy("suspend wake=key alarm=600\noff after=1800");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    suspendNow();
    for (int i = 0; i < 2; i++) {
        DummyClockAdvance(600ULL * NS_PER_S);
        sendCtl("ready");
        runMs(150);
    }
    DummyClockAdvance(601ULL * NS_PER_S);
    (void)events();
    PowerTick();
    TEST_ASSERT_NOT_NULL(strstr(events(), "enter off"));
}

TEST(power_suspend, AKeyWakeRestartsTheIdleCount) {
    setPolicy("standby idle=60\nsuspend wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    run(30);
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    (void)events();
    run(40);
    TEST_ASSERT_EQUAL_STRING("", events());
    run(25);
    TEST_ASSERT_EQUAL_STRING("enter standby\n", events());
}

TEST(power_suspend, AnIdleDeadlineOfALaterModeArmsTheRemainingTime) {
    setPolicy("suspend wake=key\noff idle=100");
    run(30);
    suspendNow();
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(68ULL * 1000000ULL, fake.arm.timerUs);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(71ULL * 1000000ULL, fake.arm.timerUs);
}

TEST(power_suspend, ACtlAlarmReplacesTheNextAlarm) {
    setPolicy("suspend wake=key alarm=60");
    sendCtl("alarm 30");
    suspendNow();
    TEST_ASSERT_EQUAL_UINT64(30ULL * 1000000ULL, fake.arm.timerUs);
}

TEST(power_suspend, AFailingBoardHookAbortsTheSuspend) {
    setPolicy("suspend wake=key");
    fake.failSuspend = -EIO;
    (void)events();
    suspendNow();
    TEST_ASSERT_EQUAL_STRING("active", modeName());
    TEST_ASSERT_EQUAL_STRING("enter suspend\nexit suspend failed\n", events());
    TEST_ASSERT_EQUAL_UINT(0, DummyPowerLightSleeps());
    TEST_ASSERT_EQUAL_STRING("BAsrAB", trace);
}

TEST(power_suspend,
     ADeepSleepFallsBackToALightSleepOnAPlatformThatCannotSleep) {
    setPolicy("sleep wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    sendCtl("mode sleep");
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_UINT(1, DummyPowerDeepSleeps());
    TEST_ASSERT_EQUAL_UINT(1, fake.sleeps);
    TEST_ASSERT_EQUAL_UINT(0, fake.suspends);
    TEST_ASSERT_EQUAL_STRING("active", modeName());
}

TEST(power_suspend, CapsRailsAndModeReadBack) {
    char buf[160];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/caps", buf, 160));
    TEST_ASSERT_EQUAL_STRING("modes active standby suspend sleep off\n"
                             "wake key standby suspend sleep\n"
                             "wake touch standby\n"
                             "cut no\n",
                             buf);
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/rails", buf, 160));
    TEST_ASSERT_EQUAL_STRING("lora on holders=lora:main\n", buf);
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/mode", buf, 160));
    TEST_ASSERT_EQUAL_INT(0, strncmp(buf, "active ", 7));
}

TEST_GROUP_RUNNER(power_suspend) {
    RUN_TEST_CASE(power_suspend,
                  ClientsSuspendInReverseOrderThenTheBoardAndResumeBackwards);
    RUN_TEST_CASE(power_suspend, ThePolicysWakeSourcesReachTheBoardHook);
    RUN_TEST_CASE(power_suspend, AKeyWakeReturnsToActiveAndQueuesAnExit);
    RUN_TEST_CASE(power_suspend, TheResumeTermChoosesTheModeAKeyWakeEnters);
    RUN_TEST_CASE(power_suspend, TheBoardNamesTheWakeWord);
    RUN_TEST_CASE(power_suspend, TheNextLadderDeadlineArmsAsTheTimer);
    RUN_TEST_CASE(power_suspend, WithNoLaterModeThereIsNoTimer);
    RUN_TEST_CASE(power_suspend,
                  ATimerWakeAtTheLadderDeadlineGoesOnToTheNextMode);
    RUN_TEST_CASE(power_suspend,
                  ALadderWakeKeepsTheHardwareDownIntoTheNextMode);
    RUN_TEST_CASE(power_suspend,
                  ALadderWakeIntoTheUsbFallbackSuspendsNothingAgain);
    RUN_TEST_CASE(power_suspend,
                  ALadderWakeWithNothingDueBringsTheHardwareBack);
    RUN_TEST_CASE(power_suspend,
                  AModeRequestAfterALadderWakeBringsTheHardwareBack);
    RUN_TEST_CASE(power_suspend, AnAlarmWakeAsksTheWappToPrepareAndSleepsAgain);
    RUN_TEST_CASE(power_suspend, TheAlarmCyclesDoNotResetTheLadderClock);
    RUN_TEST_CASE(power_suspend, AKeyWakeRestartsTheIdleCount);
    RUN_TEST_CASE(power_suspend,
                  AnIdleDeadlineOfALaterModeArmsTheRemainingTime);
    RUN_TEST_CASE(power_suspend, ACtlAlarmReplacesTheNextAlarm);
    RUN_TEST_CASE(power_suspend, AFailingBoardHookAbortsTheSuspend);
    RUN_TEST_CASE(power_suspend,
                  ADeepSleepFallsBackToALightSleepOnAPlatformThatCannotSleep);
    RUN_TEST_CASE(power_suspend, CapsRailsAndModeReadBack);
}

/***************************************/
TEST_GROUP(power_safety);
/***************************************/

TEST_SETUP(power_safety) {
    setup();
    startDevice();
}

TEST_TEAR_DOWN(power_safety) { teardown(); }

TEST(power_safety, AHeldKeyEntersOffWithNoPolicyAndNoReady) {
    TEST_ASSERT_EQUAL_INT(0, PowerNotify("main", POWER_NOTICE_KEY_HELD));
    TEST_ASSERT_EQUAL_STRING("enter off\n", events());
    run(2);
    TEST_ASSERT_EQUAL_UINT(0, fake.poweroffs);
    run(2);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, AReadyShortensTheGrace) {
    PowerNotify("main", POWER_NOTICE_LOW_BATTERY);
    sendCtl("ready");
    runMs(150);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, ThePolicyGraceCannotPassThreeSecondsForASafetyOff) {
    setPolicy("off after=3600 grace=30");
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, AShorterPolicyGraceStands) {
    setPolicy("off after=3600 grace=1");
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(2);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, AHungWappCannotStopTheOffAndNoPolicyDisablesIt) {
    setPolicy("standby idle=5\nsuspend wake=key");
    detach(&owner);
    PowerNotify("main", POWER_NOTICE_LOW_BATTERY);
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, AnUnknownDeviceNameIsAnError) {
    TEST_ASSERT_EQUAL_INT(-ENOENT, PowerNotify("aux", POWER_NOTICE_KEY_HELD));
    TEST_ASSERT_EQUAL_INT(-ENOENT, PowerNotify(NULL, POWER_NOTICE_KEY_HELD));
}

TEST(power_safety, ASafetyOffReplacesAPendingSuspend) {
    setPolicy("suspend wake=key");
    sendCtl("mode suspend");
    (void)events();
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    TEST_ASSERT_EQUAL_STRING("enter off\n", events());
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
    TEST_ASSERT_EQUAL_UINT(0, fake.suspends);
}

TEST(power_safety, OffThatCannotCutPowerSleepsDeeplyWithOnlyTheKey) {
    fake.ext = true;
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_UINT(0, fake.poweroffs);
    TEST_ASSERT_EQUAL_UINT(1, fake.sleeps);
    TEST_ASSERT_TRUE(fake.arm.powerKeyOnly);
    TEST_ASSERT_EQUAL_UINT(0, fake.arm.wakeMask);
    TEST_ASSERT_EQUAL_UINT64(0, fake.arm.timerUs);
    TEST_ASSERT_EQUAL_UINT(1, DummyPowerDeepSleeps());
}

TEST(power_safety, TheFallbackDropsThePolicysWakeSourcesAndTimer) {
    setPolicy("sleep wake=key alarm=60\noff after=3600");
    fake.ext = true;
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.sleeps);
    TEST_ASSERT_TRUE(fake.arm.powerKeyOnly);
    TEST_ASSERT_EQUAL_UINT(0, fake.arm.wakeMask);
    TEST_ASSERT_EQUAL_UINT64(0, fake.arm.timerUs);
    TEST_ASSERT_EQUAL_UINT64(0, DummyPowerLastTimerUs());
}

TEST(power_safety, OffThatCutsPowerEvenWithExternalPowerPowersOff) {
    teardown();
    setup();
    power_device_desc_t d = mainDesc();
    d.cutsWithExternalPower = true;
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    fake.ext = true;
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
}

TEST(power_safety, APowerOffThatReturnsFallsBackToDeepSleep) {
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_UINT(1, fake.poweroffs);
    TEST_ASSERT_EQUAL_UINT(1, fake.sleeps);
    TEST_ASSERT_TRUE(fake.arm.powerKeyOnly);
}

/* A board client that holds a lock across a sleep must see one suspend. */
TEST(power_safety, APowerOffThatReturnsSuspendsTheClientsOnce) {
    power_client_t a = {"a", clientA, clientB, NULL};
    TEST_ASSERT_EQUAL_INT(0, PowerClientRegister(&a));
    trace[0] = '\0';
    PowerNotify("main", POWER_NOTICE_KEY_HELD);
    run(4);
    TEST_ASSERT_EQUAL_STRING("AodrB", trace);
}

TEST_GROUP_RUNNER(power_safety) {
    RUN_TEST_CASE(power_safety, AHeldKeyEntersOffWithNoPolicyAndNoReady);
    RUN_TEST_CASE(power_safety, AReadyShortensTheGrace);
    RUN_TEST_CASE(power_safety,
                  ThePolicyGraceCannotPassThreeSecondsForASafetyOff);
    RUN_TEST_CASE(power_safety, AShorterPolicyGraceStands);
    RUN_TEST_CASE(power_safety, AHungWappCannotStopTheOffAndNoPolicyDisablesIt);
    RUN_TEST_CASE(power_safety, AnUnknownDeviceNameIsAnError);
    RUN_TEST_CASE(power_safety, ASafetyOffReplacesAPendingSuspend);
    RUN_TEST_CASE(power_safety,
                  OffThatCannotCutPowerSleepsDeeplyWithOnlyTheKey);
    RUN_TEST_CASE(power_safety, TheFallbackDropsThePolicysWakeSourcesAndTimer);
    RUN_TEST_CASE(power_safety, OffThatCutsPowerEvenWithExternalPowerPowersOff);
    RUN_TEST_CASE(power_safety, APowerOffThatReturnsFallsBackToDeepSleep);
    RUN_TEST_CASE(power_safety, APowerOffThatReturnsSuspendsTheClientsOnce);
}

/***************************************/
TEST_GROUP(power_register);
/***************************************/

TEST_SETUP(power_register) { setup(); }

TEST_TEAR_DOWN(power_register) { teardown(); }

TEST(power_register, RegistrationRejectsBadDescriptions) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(NULL));
    d.name = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    d = mainDesc();
    d.name = "a b";
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    d = mainDesc();
    d.ops = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    d = mainDesc();
    d.modes = POWER_BIT(POWER_OFF);
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    d = mainDesc();
    d.wakeCount = POWER_WAKE_MAX + 1;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    d = mainDesc();
    d.wakes = NULL;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
    power_ops_t partial = fakeOps;
    partial.suspend = NULL;
    d = mainDesc();
    d.ops = &partial;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerDeviceRegister(&d));
}

TEST(power_register, ADuplicateNameIsRefused) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_EQUAL_INT(-EEXIST, PowerDeviceRegister(&d));
}

TEST(power_register, ClientRegistrationRejectsNothingAndFillsUp) {
    power_client_t empty = {"x", NULL, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerClientRegister(NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PowerClientRegister(&empty));
    power_client_t a = {"a", clientA, NULL, NULL};
    for (int i = 0; i < CONFIG_WANTED_POWER_MAX_CLIENTS; i++)
        TEST_ASSERT_EQUAL_INT(0, PowerClientRegister(&a));
    TEST_ASSERT_EQUAL_INT(-ENOSPC, PowerClientRegister(&a));
}

TEST(power_register, ATickWithNoDeviceIsHarmless) {
    PowerTick();
    TEST_PASS();
}

TEST(power_register, TheLadderComesBackAfterADeepSleep) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    setPolicy("sleep wake=key\noff after=3600");
    DummyPowerWakeSet(PLAT_POWER_WAKE_TIMER);
    sendCtl("mode sleep");
    sendCtl("ready");
    runMs(150);
    detach(&owner);
    PowerDevicesReset();
    trace[0] = '\0';
    DummyPowerBootSet(PLAT_POWER_BOOT_TIMER, 3600ULL * 1000000ULL);
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    char buf[64];
    TEST_ASSERT_GREATER_THAN(0, readVia(&owner, "main/policy", buf, 64));
    TEST_ASSERT_EQUAL_STRING("sleep wake=key\noff after=3600\n", buf);
    TEST_ASSERT_EQUAL_STRING("sleep", modeName());
    PowerTick();
    TEST_ASSERT_NOT_NULL(strstr(events(), "enter off"));
}

TEST(power_register, AColdBootIgnoresTheSavedLadder) {
    power_device_desc_t d = mainDesc();
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    setPolicy("sleep wake=key");
    DummyPowerWakeSet(PLAT_POWER_WAKE_EXTERNAL);
    sendCtl("mode sleep");
    sendCtl("ready");
    runMs(150);
    detach(&owner);
    PowerDevicesReset();
    DummyPowerBootSet(PLAT_POWER_BOOT_COLD, 0);
    TEST_ASSERT_EQUAL_INT(0, PowerDeviceRegister(&d));
    TEST_ASSERT_TRUE(attach(&owner, CONTROL));
    char buf[16];
    TEST_ASSERT_EQUAL_INT(0, readVia(&owner, "main/policy", buf, 16));
    TEST_ASSERT_EQUAL_STRING("active", modeName());
}

TEST_GROUP_RUNNER(power_register) {
    RUN_TEST_CASE(power_register, RegistrationRejectsBadDescriptions);
    RUN_TEST_CASE(power_register, ADuplicateNameIsRefused);
    RUN_TEST_CASE(power_register, ClientRegistrationRejectsNothingAndFillsUp);
    RUN_TEST_CASE(power_register, ATickWithNoDeviceIsHarmless);
    RUN_TEST_CASE(power_register, TheLadderComesBackAfterADeepSleep);
    RUN_TEST_CASE(power_register, AColdBootIgnoresTheSavedLadder);
}
