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
#include <vfs-rtc.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>

/* /dev/rtc: the grant, the nodes, the write order, the system clock and the
 * boot step, over a fake chip device and the dummy platform clock. */

#define NS_PER_S 1000000000ULL
#define T_NOW 1791300000UL
#define T_OTHER 1800000000UL

typedef struct fake_t {
    uint32_t sec;
    bool valid;
    int getRc;
    int setRc;
    int getCalls;
    int setCalls;
    uint32_t sysAtSet; /* the system clock, in seconds, while set ran */
} fake_t;

static fake_t chip;
static fake_t aux;

static uint32_t sysSeconds(void) {
    plat_timestamp_t ns = 0;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &ns);
    return (uint32_t)(ns / NS_PER_S);
}

static int fakeGet(void *ctx, uint32_t *sec, bool *valid) {
    fake_t *f = ctx;
    f->getCalls++;
    if (f->getRc < 0)
        return f->getRc;
    *sec = f->sec;
    *valid = f->valid;
    return 0;
}

static int fakeSet(void *ctx, uint32_t sec) {
    fake_t *f = ctx;
    f->setCalls++;
    if (f->setRc < 0)
        return f->setRc;
    f->sysAtSet = sysSeconds();
    f->sec = sec;
    f->valid = true;
    return 0;
}

static const rtc_ops_t fakeOps = {fakeGet, fakeSet};

static int registerChip(const char *name, fake_t *f) {
    rtc_device_desc_t d = {name, &fakeOps, f};
    return RtcDeviceRegister(&d);
}

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;
static peer_t other;

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsRtcInit(NULL, options);
    if (p->drv == NULL) {
        VfsDestroy(&p->vfs);
        return false;
    }
    DevFs_Register(p->vfs, "rtc", p->drv);
    return true;
}

static void detach(peer_t *p) {
    if (p->drv != NULL)
        VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static void setupRtc(void) {
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
    memset(&chip, 0, sizeof(chip));
    memset(&aux, 0, sizeof(aux));
}

static void teardownRtc(void) {
    detach(&other);
    detach(&owner);
    RtcDevicesReset();
}

/* `path` is relative to /dev/rtc. */
static int readVia(peer_t *p, const char *path, char *buf, size_t bufLen) {
    char full[64];
    snprintf(full, sizeof(full), "/dev/rtc%s%s", path[0] ? "/" : "", path);
    int fd = VfsOpen(p->vfs, full, VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(p->vfs, fd, buf, bufLen - 1);
    VfsClose(p->vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int openVia(peer_t *p, const char *path, int flags) {
    char full[64];
    snprintf(full, sizeof(full), "/dev/rtc/%s", path);
    int fd = VfsOpen(p->vfs, full, flags);
    if (fd >= 0)
        VfsClose(p->vfs, fd);
    return fd;
}

static int writeVia(peer_t *p, const char *path, const char *payload) {
    char full[64];
    snprintf(full, sizeof(full), "/dev/rtc/%s", path);
    int fd = VfsOpen(p->vfs, full, VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(p->vfs, fd, payload, strlen(payload));
    VfsClose(p->vfs, fd);
    return n;
}

static int writeTime(peer_t *p, const char *payload) {
    return writeVia(p, "main/time", payload);
}

static void expectLine(peer_t *p, const char *path, const char *want) {
    char buf[64];
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)strlen(want),
                                  readVia(p, path, buf, sizeof(buf)), path);
    TEST_ASSERT_EQUAL_STRING(want, buf);
}

static void expectReadError(peer_t *p, const char *path, int err) {
    char buf[64];
    TEST_ASSERT_EQUAL_INT_MESSAGE(err, readVia(p, path, buf, sizeof(buf)),
                                  path);
}

static int listDir(peer_t *p, const char *path, char *out, size_t outLen) {
    char full[64];
    snprintf(full, sizeof(full), "/dev/rtc%s%s", path[0] ? "/" : "", path);
    int fd = VfsOpen(p->vfs, full, VFS_O_RDONLY);
    if (fd < 0)
        return fd;

    uint8_t buf[256];
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
                ent.d_namlen < outLen - strlen(out) - 1
                    ? ent.d_namlen
                    : outLen - strlen(out) - 1);
        off += ent.d_namlen;
    }
    return 0;
}

/***************************************/
TEST_GROUP(rtc_register);
/***************************************/

TEST_SETUP(rtc_register) { setupRtc(); }
TEST_TEAR_DOWN(rtc_register) { teardownRtc(); }

TEST(rtc_register, RejectsABadDescription) {
    rtc_device_desc_t none = {NULL, &fakeOps, &chip};
    rtc_device_desc_t noOps = {"main", NULL, &chip};
    rtc_ops_t noGet = {NULL, fakeSet};
    rtc_ops_t noSet = {fakeGet, NULL};
    rtc_device_desc_t g = {"main", &noGet, &chip};
    rtc_device_desc_t s = {"main", &noSet, &chip};
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcDeviceRegister(NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcDeviceRegister(&none));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcDeviceRegister(&noOps));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcDeviceRegister(&g));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcDeviceRegister(&s));
}

TEST(rtc_register, NamesFollowTheTokenGrammar) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, registerChip("", &chip));
    TEST_ASSERT_EQUAL_INT(-EINVAL, registerChip("a b", &chip));
    TEST_ASSERT_EQUAL_INT(-EINVAL, registerChip("a/b", &chip));
    TEST_ASSERT_EQUAL_INT(-EINVAL, registerChip("a.b", &chip));
    TEST_ASSERT_EQUAL_INT(-EINVAL, registerChip("abcdefghijklmnop", &chip));
    TEST_ASSERT_EQUAL_INT(0, registerChip("abcdefghijklmno", &chip));
    TEST_ASSERT_EQUAL_INT(0, registerChip("A_z-9", &aux));
}

TEST(rtc_register, ARepeatedNameIsRefused) {
    TEST_ASSERT_EQUAL_INT(0, registerChip("main", &chip));
    TEST_ASSERT_EQUAL_INT(-EEXIST, registerChip("main", &aux));
}

TEST(rtc_register, AFullTableIsRefused) {
    char name[8];
    for (int i = 0; i < CONFIG_WANTED_RTC_MAX_DEVICES; i++) {
        snprintf(name, sizeof(name), "d%d", i);
        TEST_ASSERT_EQUAL_INT(0, registerChip(name, &chip));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, registerChip("extra", &chip));
}

TEST(rtc_register, TokenGrammarBoundaries) {
    const char *good[] = {"A", "Z", "a", "z", "0", "9", "_", "-"};
    const char *bad[] = {"@", "[", "`", "{", "/", ":", " ", ".", "\n"};
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, registerChip(good[i], &chip), good[i]);
        RtcDevicesReset();
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-EINVAL, registerChip(bad[i], &chip),
                                      bad[i]);
        RtcDevicesReset();
    }
}

TEST(rtc_register, AFailedGrantLeavesTheNameMainFree) {
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=nope"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=ma"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=mai"));
    TEST_ASSERT_EQUAL_INT(0, registerChip("main", &chip));
}

TEST(rtc_register, ALongestNameWorksEndToEnd) {
    TEST_ASSERT_EQUAL_INT(0, registerChip("abcdefghijklmno", &chip));
    chip.valid = true;
    chip.sec = T_NOW;
    TEST_ASSERT_TRUE(attach(&owner, "devices=abcdefghijklmno"));
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir(&owner, "", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("abcdefghijklmno", names);
    expectLine(&owner, "abcdefghijklmno/time", "1791300000\n");
}

TEST_GROUP_RUNNER(rtc_register) {
    RUN_TEST_CASE(rtc_register, RejectsABadDescription);
    RUN_TEST_CASE(rtc_register, NamesFollowTheTokenGrammar);
    RUN_TEST_CASE(rtc_register, ARepeatedNameIsRefused);
    RUN_TEST_CASE(rtc_register, AFullTableIsRefused);
    RUN_TEST_CASE(rtc_register, TokenGrammarBoundaries);
    RUN_TEST_CASE(rtc_register, AFailedGrantLeavesTheNameMainFree);
    RUN_TEST_CASE(rtc_register, ALongestNameWorksEndToEnd);
}

/***************************************/
TEST_GROUP(rtc_grant);
/***************************************/

TEST_SETUP(rtc_grant) {
    setupRtc();
    registerChip("main", &chip);
    registerChip("aux", &aux);
}
TEST_TEAR_DOWN(rtc_grant) { teardownRtc(); }

TEST(rtc_grant, ExposesTheThreeNodesOfTheGrantedDevice) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir(&owner, "", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("main", names);
    TEST_ASSERT_EQUAL_INT(0, listDir(&owner, "main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("time,status,source", names);
}

TEST(rtc_grant, ADeviceThatWasNotGrantedIsAbsent) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "aux", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "aux/time", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "aux/status", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "aux/source", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "nope/time", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "main/alarm", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          openVia(&owner, "main/time/x", VFS_O_RDONLY));
}

TEST(rtc_grant, TheOtherDeviceCanBeGrantedInstead) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=aux"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, openVia(&owner, "main/time", VFS_O_RDONLY));
    TEST_ASSERT_TRUE(openVia(&owner, "aux/time", VFS_O_RDONLY) >= 0);
}

TEST(rtc_grant, BadGrantsFailTheLaunch) {
    TEST_ASSERT_NULL(VfsRtcInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, ""));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices="));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=,set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "main"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "device=main"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "set,devices=main"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,,set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,write"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,set,set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,aux"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main,aux,set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=m ain"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=main;set"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=abcdefghijklmnop"));
}

TEST(rtc_grant, ADeviceTheBoardDidNotRegisterFailsTheLaunch) {
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=nope"));
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=nope,set"));
}

TEST(rtc_grant, StatusAndSourceAreReadOnly) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_TRUE(openVia(&owner, "main/status", VFS_O_RDONLY) >= 0);
    TEST_ASSERT_TRUE(openVia(&owner, "main/source", VFS_O_RDONLY) >= 0);
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          openVia(&owner, "main/status", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EACCES, openVia(&owner, "main/status", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          openVia(&owner, "main/source", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EACCES, openVia(&owner, "main/source", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(
        -EACCES, openVia(&owner, "main/source", VFS_O_RDONLY | VFS_O_TRUNC));
}

TEST(rtc_grant, TimeIsReadOnlyWithoutTheSetClause) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_TRUE(openVia(&owner, "main/time", VFS_O_RDONLY) >= 0);
    TEST_ASSERT_EQUAL_INT(-EACCES, openVia(&owner, "main/time", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EACCES, openVia(&owner, "main/time", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(-EACCES, writeTime(&owner, "1791300000"));
}

TEST(rtc_grant, TimeIsWritableWithTheSetClause) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_TRUE(openVia(&owner, "main/time", VFS_O_WRONLY) >= 0);
    TEST_ASSERT_TRUE(openVia(&owner, "main/time", VFS_O_RDWR) >= 0);
}

TEST(rtc_grant, ManyWappsMayHoldTheGrantAndSet) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_TRUE(attach(&other, "devices=main,set"));
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1791300000"));
    TEST_ASSERT_EQUAL_INT(10, writeTime(&other, "1800000000"));
    expectLine(&owner, "main/time", "1800000000\n");
    expectLine(&other, "main/time", "1800000000\n");
}

TEST(rtc_grant, AReaderNeverChangesTheClock) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_EQUAL_INT(-EACCES, writeTime(&owner, "1791300000 sntp"));
    TEST_ASSERT_EQUAL_INT(0, chip.setCalls);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST_GROUP_RUNNER(rtc_grant) {
    RUN_TEST_CASE(rtc_grant, ExposesTheThreeNodesOfTheGrantedDevice);
    RUN_TEST_CASE(rtc_grant, ADeviceThatWasNotGrantedIsAbsent);
    RUN_TEST_CASE(rtc_grant, TheOtherDeviceCanBeGrantedInstead);
    RUN_TEST_CASE(rtc_grant, BadGrantsFailTheLaunch);
    RUN_TEST_CASE(rtc_grant, ADeviceTheBoardDidNotRegisterFailsTheLaunch);
    RUN_TEST_CASE(rtc_grant, StatusAndSourceAreReadOnly);
    RUN_TEST_CASE(rtc_grant, TimeIsReadOnlyWithoutTheSetClause);
    RUN_TEST_CASE(rtc_grant, TimeIsWritableWithTheSetClause);
    RUN_TEST_CASE(rtc_grant, ManyWappsMayHoldTheGrantAndSet);
    RUN_TEST_CASE(rtc_grant, AReaderNeverChangesTheClock);
}

/***************************************/
TEST_GROUP(rtc_nodes);
/***************************************/

TEST_SETUP(rtc_nodes) {
    setupRtc();
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
    registerChip("aux", &aux);
}
TEST_TEAR_DOWN(rtc_nodes) { teardownRtc(); }

TEST(rtc_nodes, TimeReadsTheDeviceAsDecimalSecondsAndANewline) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/time", "1791300000\n");
}

TEST(rtc_nodes, TimeIsTakenFromTheDeviceAtEachRead) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/time", "1791300000\n");
    chip.sec = T_NOW + 61;
    expectLine(&owner, "main/time", "1791300061\n");
}

TEST(rtc_nodes, TimeIsEioWhileTheDeviceIsInvalid) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    chip.valid = false;
    expectReadError(&owner, "main/time", -EIO);
}

TEST(rtc_nodes, TimeIsEioWhenTheDeviceDoesNotAnswer) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    chip.getRc = -ETIMEDOUT;
    expectReadError(&owner, "main/time", -EIO);
}

TEST(rtc_nodes, StatusReadsValidOrInvalid) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "valid\n");
    chip.valid = false;
    expectLine(&owner, "main/status", "invalid\n");
}

TEST(rtc_nodes, StatusIsEioWhenTheDeviceDoesNotAnswer) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    chip.getRc = -EIO;
    expectReadError(&owner, "main/status", -EIO);
}

TEST(rtc_nodes, ASuccessfulWriteMakesAnInvalidDeviceValid) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    chip.valid = false;
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1791300000"));
    expectLine(&owner, "main/status", "valid\n");
}

TEST(rtc_nodes, SourceStartsAsNoneAndNamesTheLastWriter) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    expectLine(&owner, "main/source", "none\n");
    const char *sources[] = {"rtc", "sntp", "server", "manual"};
    for (size_t i = 0; i < 4; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "1791300000 %s", sources[i]);
        TEST_ASSERT_EQUAL_INT((int)strlen(payload), writeTime(&owner, payload));
        char want[16];
        snprintf(want, sizeof(want), "%s\n", sources[i]);
        expectLine(&owner, "main/source", want);
    }
}

TEST(rtc_nodes, SourceDefaultsToManual) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    writeTime(&owner, "1791300000");
    expectLine(&owner, "main/source", "manual\n");
}

TEST(rtc_nodes, ALineArrivesInOneReadAndThenEnds) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    int fd = VfsOpen(owner.vfs, "/dev/rtc/main/time", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[32];
    TEST_ASSERT_EQUAL_INT(11, VfsRead(owner.vfs, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, VfsRead(owner.vfs, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, VfsRead(owner.vfs, fd, buf, sizeof(buf)));
    VfsClose(owner.vfs, fd);
}

TEST(rtc_nodes, AShortBufferGetsThePartThatFits) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    int fd = VfsOpen(owner.vfs, "/dev/rtc/main/time", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(4, VfsRead(owner.vfs, fd, buf, 4));
    TEST_ASSERT_EQUAL_STRING_LEN("1791", buf, 4);
    VfsClose(owner.vfs, fd);
}

TEST(rtc_nodes, ANewOpenReadsAfresh) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "valid\n");
    expectLine(&owner, "main/status", "valid\n");
}

TEST(rtc_nodes, DirectoriesCannotBeReadOrWritten) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EISDIR, readVia(&owner, "", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EISDIR, readVia(&owner, "main", buf, sizeof(buf)));
}

TEST_GROUP_RUNNER(rtc_nodes) {
    RUN_TEST_CASE(rtc_nodes, TimeReadsTheDeviceAsDecimalSecondsAndANewline);
    RUN_TEST_CASE(rtc_nodes, TimeIsTakenFromTheDeviceAtEachRead);
    RUN_TEST_CASE(rtc_nodes, TimeIsEioWhileTheDeviceIsInvalid);
    RUN_TEST_CASE(rtc_nodes, TimeIsEioWhenTheDeviceDoesNotAnswer);
    RUN_TEST_CASE(rtc_nodes, StatusReadsValidOrInvalid);
    RUN_TEST_CASE(rtc_nodes, StatusIsEioWhenTheDeviceDoesNotAnswer);
    RUN_TEST_CASE(rtc_nodes, ASuccessfulWriteMakesAnInvalidDeviceValid);
    RUN_TEST_CASE(rtc_nodes, SourceStartsAsNoneAndNamesTheLastWriter);
    RUN_TEST_CASE(rtc_nodes, SourceDefaultsToManual);
    RUN_TEST_CASE(rtc_nodes, ALineArrivesInOneReadAndThenEnds);
    RUN_TEST_CASE(rtc_nodes, AShortBufferGetsThePartThatFits);
    RUN_TEST_CASE(rtc_nodes, ANewOpenReadsAfresh);
    RUN_TEST_CASE(rtc_nodes, DirectoriesCannotBeReadOrWritten);
}

/***************************************/
TEST_GROUP(rtc_write);
/***************************************/

TEST_SETUP(rtc_write) {
    setupRtc();
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
    registerChip("aux", &aux);
    attach(&owner, "devices=main,set");
}
TEST_TEAR_DOWN(rtc_write) { teardownRtc(); }

TEST(rtc_write, ABadLineIsEinvalAndChangesNothing) {
    const char *bad[] = {
        "",           "x",  "1791300000 nope", "946684799",
        "4102444800", "-1", "1791300000 sn",   "1791300000 sntp x"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(-EINVAL, writeTime(&owner, bad[i]),
                                      bad[i]);
    }
    TEST_ASSERT_EQUAL_INT(0, chip.setCalls);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    expectLine(&owner, "main/source", "none\n");
}

TEST(rtc_write, ALineLongerThanAnyValidOneIsEinval) {
    char big[128];
    memset(big, '1', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-EINVAL, writeTime(&owner, big));
    TEST_ASSERT_EQUAL_INT(0, chip.setCalls);
}

TEST(rtc_write, ATrailingNewlineIsAccepted) {
    TEST_ASSERT_EQUAL_INT(11, writeTime(&owner, "1791300000\n"));
    TEST_ASSERT_EQUAL_UINT32(T_NOW, chip.sec);
}

TEST(rtc_write, TheWriteReturnsTheByteCount) {
    TEST_ASSERT_EQUAL_INT(15, writeTime(&owner, "1791300000 sntp"));
}

TEST(rtc_write, TheDeviceIsSetBeforeTheSystemClock) {
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1800000000"));
    TEST_ASSERT_EQUAL_INT(1, chip.setCalls);
    TEST_ASSERT_TRUE(chip.sysAtSet != T_OTHER);
    TEST_ASSERT_EQUAL_UINT32(T_OTHER, sysSeconds());
}

TEST(rtc_write, TheQualityByteFollowsTheSource) {
    struct {
        const char *line;
        uint8_t quality;
    } cases[] = {
        {"1791300000 rtc", WANTED_CLOCK_HARDWARE_RTC},
        {"1791300000 sntp", WANTED_CLOCK_SNTP_CALIBRATED},
        {"1791300000 server", WANTED_CLOCK_SIMPLE_CALIBRATION},
        {"1791300000 manual", WANTED_CLOCK_SIMPLE_CALIBRATION},
        {"1791300000", WANTED_CLOCK_SIMPLE_CALIBRATION},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
        TEST_ASSERT_TRUE(writeTime(&owner, cases[i].line) > 0);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(cases[i].quality,
                                        WantedGetClockQuality(), cases[i].line);
    }
}

TEST(rtc_write, ADeviceFailureIsEioAndKeepsEverything) {
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1791300000"));
    uint32_t sys = sysSeconds();
    chip.setRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime(&owner, "1800000000 sntp"));
    TEST_ASSERT_EQUAL_UINT32(T_NOW, chip.sec);
    TEST_ASSERT_TRUE(sysSeconds() - sys < 5);
    expectLine(&owner, "main/source", "manual\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION,
                            WantedGetClockQuality());
}

TEST(rtc_write, ASystemClockFailureIsEioAndTheDeviceKeepsTheNewTime) {
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1791300000"));
    DummyClockFailSet(-ENOSYS);
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime(&owner, "1800000000 sntp"));
    TEST_ASSERT_EQUAL_UINT32(T_OTHER, chip.sec);
    expectLine(&owner, "main/source", "manual\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION,
                            WantedGetClockQuality());
}

TEST(rtc_write, ARefusedSystemClockIsEpermAndTheDeviceKeepsTheNewTime) {
    TEST_ASSERT_EQUAL_INT(10, writeTime(&owner, "1791300000"));
    DummyClockFailSet(-EPERM);
    TEST_ASSERT_EQUAL_INT(-EPERM, writeTime(&owner, "1800000000 sntp"));
    TEST_ASSERT_EQUAL_UINT32(T_OTHER, chip.sec);
    expectLine(&owner, "main/source", "manual\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION,
                            WantedGetClockQuality());
}

TEST(rtc_write, ANonMainDeviceLeavesTheSystemClockAndTheQuality) {
    TEST_ASSERT_TRUE(attach(&other, "devices=aux,set"));
    uint32_t before = sysSeconds();
    TEST_ASSERT_EQUAL_INT(15, writeVia(&other, "aux/time", "1791300000 sntp"));
    TEST_ASSERT_EQUAL_UINT32(T_NOW, aux.sec);
    TEST_ASSERT_TRUE(sysSeconds() - before < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    expectLine(&other, "aux/source", "sntp\n");
    expectLine(&owner, "main/source", "none\n");
}

TEST_GROUP_RUNNER(rtc_write) {
    RUN_TEST_CASE(rtc_write, ABadLineIsEinvalAndChangesNothing);
    RUN_TEST_CASE(rtc_write, ALineLongerThanAnyValidOneIsEinval);
    RUN_TEST_CASE(rtc_write, ATrailingNewlineIsAccepted);
    RUN_TEST_CASE(rtc_write, TheWriteReturnsTheByteCount);
    RUN_TEST_CASE(rtc_write, TheDeviceIsSetBeforeTheSystemClock);
    RUN_TEST_CASE(rtc_write, TheQualityByteFollowsTheSource);
    RUN_TEST_CASE(rtc_write, ADeviceFailureIsEioAndKeepsEverything);
    RUN_TEST_CASE(rtc_write,
                  ASystemClockFailureIsEioAndTheDeviceKeepsTheNewTime);
    RUN_TEST_CASE(rtc_write,
                  ARefusedSystemClockIsEpermAndTheDeviceKeepsTheNewTime);
    RUN_TEST_CASE(rtc_write, ANonMainDeviceLeavesTheSystemClockAndTheQuality);
}

/***************************************/
TEST_GROUP(rtc_software);
/***************************************/

TEST_SETUP(rtc_software) { setupRtc(); }
TEST_TEAR_DOWN(rtc_software) { teardownRtc(); }

TEST(rtc_software, EveryEngineOffersMainWithoutABoardDevice) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_TRUE(openVia(&owner, "main/time", VFS_O_RDONLY) >= 0);
}

TEST(rtc_software, ItIsInvalidUntilTheFirstWrite) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    expectLine(&owner, "main/status", "invalid\n");
    expectLine(&owner, "main/source", "none\n");
    expectReadError(&owner, "main/time", -EIO);
}

TEST(rtc_software, AWriteSetsTheSystemClockAndMakesItValid) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_EQUAL_INT(15, writeTime(&owner, "1791300000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    expectLine(&owner, "main/status", "valid\n");
    expectLine(&owner, "main/source", "sntp\n");
    char buf[32];
    TEST_ASSERT_TRUE(readVia(&owner, "main/time", buf, sizeof(buf)) > 0);
    TEST_ASSERT_TRUE((uint32_t)atol(buf) - T_NOW < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SNTP_CALIBRATED,
                            WantedGetClockQuality());
}

TEST(rtc_software, ItFollowsTheSystemClockAfterTheWrite) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    writeTime(&owner, "1791300000");
    DummyClockAdvance(100 * NS_PER_S);
    char buf[32];
    readVia(&owner, "main/time", buf, sizeof(buf));
    TEST_ASSERT_TRUE((uint32_t)atol(buf) - (T_NOW + 100) < 5);
}

TEST(rtc_software, AFailedSystemClockSetLeavesItInvalid) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    DummyClockFailSet(-ENOSYS);
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime(&owner, "1791300000 sntp"));
    expectLine(&owner, "main/status", "invalid\n");
    expectLine(&owner, "main/source", "none\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST(rtc_software, ABoardMainTakesTheNameFirst) {
    TEST_ASSERT_EQUAL_INT(0, registerChip("main", &chip));
    chip.sec = T_NOW;
    chip.valid = true;
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "valid\n");
    TEST_ASSERT_EQUAL_INT(1, chip.getCalls);
}

TEST_GROUP_RUNNER(rtc_software) {
    RUN_TEST_CASE(rtc_software, EveryEngineOffersMainWithoutABoardDevice);
    RUN_TEST_CASE(rtc_software, ItIsInvalidUntilTheFirstWrite);
    RUN_TEST_CASE(rtc_software, AWriteSetsTheSystemClockAndMakesItValid);
    RUN_TEST_CASE(rtc_software, ItFollowsTheSystemClockAfterTheWrite);
    RUN_TEST_CASE(rtc_software, AFailedSystemClockSetLeavesItInvalid);
    RUN_TEST_CASE(rtc_software, ABoardMainTakesTheNameFirst);
}

/***************************************/
TEST_GROUP(rtc_boot);
/***************************************/

TEST_SETUP(rtc_boot) { setupRtc(); }
TEST_TEAR_DOWN(rtc_boot) { teardownRtc(); }

TEST(rtc_boot, AValidMainSetsTheSystemClockTheQualityAndTheSource) {
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
    TEST_ASSERT_TRUE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/source", "rtc\n");
    expectLine(&owner, "main/status", "valid\n");
}

TEST(rtc_boot, AnInvalidMainLeavesTheClockAndTheQuality) {
    chip.sec = T_NOW;
    chip.valid = false;
    registerChip("main", &chip);
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/source", "none\n");
}

TEST(rtc_boot, AMainThatDoesNotAnswerLeavesTheClockAndTheQuality) {
    chip.sec = T_NOW;
    chip.valid = true;
    chip.getRc = -EIO;
    registerChip("main", &chip);
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/source", "none\n");
}

TEST(rtc_boot, AFailedSystemClockSetLeavesTheQualityAndTheSource) {
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
    DummyClockFailSet(-EPERM);
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/source", "none\n");
}

TEST(rtc_boot, TheSoftwareMainChangesNothing) {
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "invalid\n");
    expectLine(&owner, "main/source", "none\n");
}

TEST(rtc_boot, OnlyMainDrivesTheSystemClock) {
    aux.sec = T_NOW;
    aux.valid = true;
    registerChip("aux", &aux);
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach(&owner, "devices=aux"));
    expectLine(&owner, "aux/source", "none\n");
}

TEST_GROUP_RUNNER(rtc_boot) {
    RUN_TEST_CASE(rtc_boot, AValidMainSetsTheSystemClockTheQualityAndTheSource);
    RUN_TEST_CASE(rtc_boot, AnInvalidMainLeavesTheClockAndTheQuality);
    RUN_TEST_CASE(rtc_boot, AMainThatDoesNotAnswerLeavesTheClockAndTheQuality);
    RUN_TEST_CASE(rtc_boot, AFailedSystemClockSetLeavesTheQualityAndTheSource);
    RUN_TEST_CASE(rtc_boot, TheSoftwareMainChangesNothing);
    RUN_TEST_CASE(rtc_boot, OnlyMainDrivesTheSystemClock);
}

/***************************************/
TEST_GROUP(rtc_ops);
/***************************************/

#define DRIVER_ID 0x64437452U /* "RtCd" read as a little-endian word */

TEST_SETUP(rtc_ops) {
    setupRtc();
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
}
TEST_TEAR_DOWN(rtc_ops) { teardownRtc(); }

static int openRaw(const char *path, int flags) {
    return owner.drv->Open(owner.drv->ctx, path, flags);
}

TEST(rtc_ops, StatTellsDirectoriesFromNodes) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    const char *paths[] = {"", "main", "main/time", "main/status",
                           "main/source"};
    for (size_t i = 0; i < 5; i++) {
        int fd = openRaw(paths[i], VFS_O_RDONLY);
        TEST_ASSERT_TRUE(fd >= 0);
        vfs_stat_t st;
        memset(&st, 0xFF, sizeof(st));
        TEST_ASSERT_EQUAL_INT(0, owner.drv->Stat(owner.drv->ctx, fd, &st));
        TEST_ASSERT_EQUAL_UINT32(DRIVER_ID, st.dev);
        TEST_ASSERT_EQUAL_UINT(i < 2 ? VFS_FILETYPE_DIRECTORY
                                     : VFS_FILETYPE_CHARACTER_DEVICE,
                               st.filetype);
        TEST_ASSERT_EQUAL_UINT32(0, st.size);
    }
}

TEST(rtc_ops, TheDriverIsADirectoryWithItsOwnId) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_DIRECTORY, owner.drv->filetype);
    TEST_ASSERT_EQUAL_UINT32(DRIVER_ID, owner.drv->bytesId);
}

TEST(rtc_ops, ABadDescriptorIsEbadfOnEveryOperation) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    int bad[] = {-1, 0, 7, 8, 100};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char buf[16];
        vfs_stat_t st;
        uint64_t cookie = 0;
        size_t used = 0;
        int fd = bad[i];
        void *c = owner.drv->ctx;
        TEST_ASSERT_EQUAL_INT(-EBADF, owner.drv->Read(c, fd, buf, sizeof(buf)));
        TEST_ASSERT_EQUAL_INT(-EBADF, owner.drv->Write(c, fd, "1", 1));
        TEST_ASSERT_EQUAL_INT(-EBADF, owner.drv->Close(c, fd));
        TEST_ASSERT_EQUAL_INT(-EBADF, owner.drv->Stat(c, fd, &st));
        TEST_ASSERT_EQUAL_INT(
            -EBADF,
            owner.drv->ReadDir(c, fd, buf, sizeof(buf), &cookie, &used));
    }
}

TEST(rtc_ops, ASecondCloseIsEbadf) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    int fd = openRaw("main/time", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(0, owner.drv->Close(owner.drv->ctx, fd));
    TEST_ASSERT_EQUAL_INT(-EBADF, owner.drv->Close(owner.drv->ctx, fd));
}

TEST(rtc_ops, TheNinthOpenIsEmfileAndAClosedSlotIsReused) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    int fds[8];
    for (int i = 0; i < 8; i++) {
        fds[i] = openRaw("main/time", VFS_O_RDONLY);
        TEST_ASSERT_TRUE(fds[i] >= 0);
    }
    TEST_ASSERT_EQUAL_INT(-EMFILE, openRaw("main/time", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(0, owner.drv->Close(owner.drv->ctx, fds[3]));
    TEST_ASSERT_EQUAL_INT(fds[3], openRaw("main/time", VFS_O_RDONLY));
}

TEST(rtc_ops, AReusedSlotStartsAfresh) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    char buf[32];
    int fd = openRaw("main/time", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(11, owner.drv->Read(owner.drv->ctx, fd, buf, 32));
    TEST_ASSERT_EQUAL_INT(0, owner.drv->Read(owner.drv->ctx, fd, buf, 32));
    owner.drv->Close(owner.drv->ctx, fd);
    fd = openRaw("main/time", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(11, owner.drv->Read(owner.drv->ctx, fd, buf, 32));
}

TEST(rtc_ops, TheWriteGuardsHoldBelowTheOpenCheck) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    void *c = owner.drv->ctx;
    int fd = openRaw("main/time", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EACCES, owner.drv->Write(c, fd, "1791300000", 10));
    TEST_ASSERT_EQUAL_INT(0, chip.setCalls);
    detach(&owner);

    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    c = owner.drv->ctx;
    fd = openRaw("main/status", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EACCES, owner.drv->Write(c, fd, "1791300000", 10));
    fd = openRaw("main/source", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EACCES, owner.drv->Write(c, fd, "1791300000", 10));
    fd = openRaw("main", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EISDIR, owner.drv->Write(c, fd, "1791300000", 10));
    fd = openRaw("", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EISDIR, owner.drv->Write(c, fd, "1791300000", 10));
    TEST_ASSERT_EQUAL_INT(0, chip.setCalls);
}

TEST(rtc_ops, ADirectoryCannotBeRead) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    char buf[8];
    int root = openRaw("", VFS_O_RDONLY);
    int dev = openRaw("main", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EISDIR,
                          owner.drv->Read(owner.drv->ctx, root, buf, 8));
    TEST_ASSERT_EQUAL_INT(-EISDIR,
                          owner.drv->Read(owner.drv->ctx, dev, buf, 8));
}

TEST(rtc_ops, ReadDirOfAFileIsEnotdir) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;
    int fd = openRaw("main/time", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOTDIR,
                          owner.drv->ReadDir(owner.drv->ctx, fd, buf,
                                             sizeof(buf), &cookie, &used));
}

TEST(rtc_ops, DirectoryEntriesCarryTheirType) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    const char *dirs[] = {"", "main"};
    for (size_t d = 0; d < 2; d++) {
        int fd = openRaw(dirs[d], VFS_O_RDONLY);
        uint8_t buf[256];
        uint64_t cookie = 0;
        size_t used = 0;
        TEST_ASSERT_EQUAL_INT(0,
                              owner.drv->ReadDir(owner.drv->ctx, fd, buf,
                                                 sizeof(buf), &cookie, &used));
        size_t off = 0;
        int seen = 0;
        while (off + sizeof(vfs_dirent_t) <= used) {
            vfs_dirent_t ent;
            memcpy(&ent, buf + off, sizeof(ent));
            TEST_ASSERT_EQUAL_UINT(d == 0 ? VFS_FILETYPE_DIRECTORY
                                          : VFS_FILETYPE_CHARACTER_DEVICE,
                                   ent.d_type);
            off += sizeof(ent) + ent.d_namlen;
            seen++;
        }
        TEST_ASSERT_EQUAL_INT(d == 0 ? 1 : 3, seen);
    }
}

TEST_GROUP_RUNNER(rtc_ops) {
    RUN_TEST_CASE(rtc_ops, StatTellsDirectoriesFromNodes);
    RUN_TEST_CASE(rtc_ops, TheDriverIsADirectoryWithItsOwnId);
    RUN_TEST_CASE(rtc_ops, ABadDescriptorIsEbadfOnEveryOperation);
    RUN_TEST_CASE(rtc_ops, ASecondCloseIsEbadf);
    RUN_TEST_CASE(rtc_ops, TheNinthOpenIsEmfileAndAClosedSlotIsReused);
    RUN_TEST_CASE(rtc_ops, AReusedSlotStartsAfresh);
    RUN_TEST_CASE(rtc_ops, TheWriteGuardsHoldBelowTheOpenCheck);
    RUN_TEST_CASE(rtc_ops, ADirectoryCannotBeRead);
    RUN_TEST_CASE(rtc_ops, ReadDirOfAFileIsEnotdir);
    RUN_TEST_CASE(rtc_ops, DirectoryEntriesCarryTheirType);
}

/***************************************/
TEST_GROUP(rtc_hosted);
/***************************************/

#define MIN_UNIX 946684800ULL
#define MAX_UNIX 4102444799ULL

TEST_SETUP(rtc_hosted) {
    setupRtc();
    DummyClockHostedSet(true);
}
TEST_TEAR_DOWN(rtc_hosted) { teardownRtc(); }

static void setHostClock(unsigned long long sec) {
    TEST_ASSERT_EQUAL_INT(
        0, PlatformClockSetTime(PLAT_CLOCKID_REALTIME, sec * NS_PER_S));
}

TEST(rtc_hosted, MainMirrorsAHostClockThatIsSet) {
    setHostClock(T_NOW);
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "valid\n");
    char buf[32];
    TEST_ASSERT_TRUE(readVia(&owner, "main/time", buf, sizeof(buf)) > 0);
    TEST_ASSERT_TRUE((uint32_t)atol(buf) - T_NOW < 5);
    expectLine(&owner, "main/source", "none\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST(rtc_hosted, MainIsInvalidWhileTheHostClockIsUnset) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "invalid\n");
    expectReadError(&owner, "main/time", -EIO);
}

TEST(rtc_hosted, TheHostClockMustLieInTheRange) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    setHostClock(MIN_UNIX - 1);
    expectLine(&owner, "main/status", "invalid\n");
    setHostClock(MIN_UNIX);
    expectLine(&owner, "main/status", "valid\n");
    setHostClock(MAX_UNIX);
    expectLine(&owner, "main/status", "valid\n");
    setHostClock(MAX_UNIX + 1);
    expectLine(&owner, "main/status", "invalid\n");
}

TEST(rtc_hosted, AnEngineThatOwnsItsClockStaysInvalidUntilWritten) {
    DummyClockHostedSet(false);
    setHostClock(T_NOW);
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    expectLine(&owner, "main/status", "invalid\n");
}

TEST(rtc_hosted, AWriteSetsTheHostClock) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_EQUAL_INT(15, writeTime(&owner, "1791300000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    expectLine(&owner, "main/status", "valid\n");
    expectLine(&owner, "main/source", "sntp\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SNTP_CALIBRATED,
                            WantedGetClockQuality());
}

TEST(rtc_hosted, AWriteWithoutPermissionIsEpermAndChangesNothing) {
    setHostClock(T_NOW);
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    DummyClockFailSet(-EPERM);
    TEST_ASSERT_EQUAL_INT(-EPERM, writeTime(&owner, "1800000000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    expectLine(&owner, "main/source", "none\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST(rtc_hosted, AnotherFailureToSetTheHostClockIsEio) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    DummyClockFailSet(-EINVAL);
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime(&owner, "1800000000 sntp"));
}

TEST(rtc_hosted, BootLeavesTheHostClockAndTheQuality) {
    setHostClock(T_NOW);
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST_GROUP_RUNNER(rtc_hosted) {
    RUN_TEST_CASE(rtc_hosted, MainMirrorsAHostClockThatIsSet);
    RUN_TEST_CASE(rtc_hosted, MainIsInvalidWhileTheHostClockIsUnset);
    RUN_TEST_CASE(rtc_hosted, TheHostClockMustLieInTheRange);
    RUN_TEST_CASE(rtc_hosted, AnEngineThatOwnsItsClockStaysInvalidUntilWritten);
    RUN_TEST_CASE(rtc_hosted, AWriteSetsTheHostClock);
    RUN_TEST_CASE(rtc_hosted, AWriteWithoutPermissionIsEpermAndChangesNothing);
    RUN_TEST_CASE(rtc_hosted, AnotherFailureToSetTheHostClockIsEio);
    RUN_TEST_CASE(rtc_hosted, BootLeavesTheHostClockAndTheQuality);
}

/***************************************/
TEST_GROUP(rtc_acceptance);
/***************************************/

TEST_SETUP(rtc_acceptance) { setupRtc(); }
TEST_TEAR_DOWN(rtc_acceptance) { teardownRtc(); }

TEST(rtc_acceptance, ASetGrantWritesTheSoftwareMain) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    TEST_ASSERT_EQUAL_INT(15, writeTime(&owner, "1791300000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    expectLine(&owner, "main/status", "valid\n");
    expectLine(&owner, "main/source", "sntp\n");
    uint8_t q = 99;
    TEST_ASSERT_EQUAL_INT(1, WantedProcReadClockQuality(NULL, &q, 1));
    TEST_ASSERT_EQUAL_UINT8(1, q);
}

TEST(rtc_acceptance, AGrantWithoutSetCannotWriteTime) {
    TEST_ASSERT_TRUE(attach(&owner, "devices=main"));
    TEST_ASSERT_EQUAL_INT(-EACCES, writeTime(&owner, "1791300000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() < 5);
}

TEST(rtc_acceptance, AChipDeviceWhoseSetFailsKeepsTheClock) {
    chip.sec = T_NOW;
    chip.valid = true;
    registerChip("main", &chip);
    RtcBoot();
    uint32_t sys = sysSeconds();
    TEST_ASSERT_TRUE(attach(&owner, "devices=main,set"));
    chip.setRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime(&owner, "1800000000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() - sys < 5);
    expectLine(&owner, "main/source", "rtc\n");
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC, WantedGetClockQuality());
}

TEST_GROUP_RUNNER(rtc_acceptance) {
    RUN_TEST_CASE(rtc_acceptance, ASetGrantWritesTheSoftwareMain);
    RUN_TEST_CASE(rtc_acceptance, AGrantWithoutSetCannotWriteTime);
    RUN_TEST_CASE(rtc_acceptance, AChipDeviceWhoseSetFailsKeepsTheClock);
}
