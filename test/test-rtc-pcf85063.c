/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <dummy-fs.h>
#include <platform.h>
#include <rtc-chip.h>
#include <rtc-time.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-rtc.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>

/* The PCF85063A driver and the chip adapter over a fake register bus. The
 * fake models the registers the driver touches: the counters, the OS flag
 * that only a write of 0 clears, and the Control_1 bits. */

#define REG_CONTROL1 0x00
#define REG_SECONDS 0x04
#define REG_ALARM_FIRST 0x0B
#define REGS 0x12
#define MAX_TX 16
#define NS_PER_S 1000000000ULL
#define T_NOW 1791300000UL

typedef struct tx_t {
    bool write;
    uint8_t reg;
    size_t len;
} tx_t;

typedef struct fake_bus_t {
    uint8_t regs[REGS];
    tx_t log[MAX_TX];
    int nTx;
    int readRc;
    int writeRc;
    bool oscDead;       /* the OS flag cannot be cleared */
    bool rolloverAfter; /* the clock ticks one second after each read */
} fake_bus_t;

static fake_bus_t fb;

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void putTime(unsigned y, unsigned mo, unsigned d, unsigned h,
                    unsigned mi, unsigned s, unsigned wd, bool os) {
    fb.regs[4] = (uint8_t)(bcd(s) | (os ? 0x80 : 0));
    fb.regs[5] = bcd(mi);
    fb.regs[6] = bcd(h);
    fb.regs[7] = bcd(d);
    fb.regs[8] = (uint8_t)wd;
    fb.regs[9] = bcd(mo);
    fb.regs[10] = bcd(y - 2000);
}

static void tick(void) {
    uint32_t t;
    rtc_tm_t tm = {
        (uint16_t)(2000 + (fb.regs[10] >> 4) * 10 + (fb.regs[10] & 15)),
        (uint8_t)((fb.regs[9] >> 4) * 10 + (fb.regs[9] & 15)),
        (uint8_t)((fb.regs[7] >> 4) * 10 + (fb.regs[7] & 15)),
        (uint8_t)((fb.regs[6] >> 4) * 10 + (fb.regs[6] & 15)),
        (uint8_t)((fb.regs[5] >> 4) * 10 + (fb.regs[5] & 15)),
        (uint8_t)(((fb.regs[4] & 0x7F) >> 4) * 10 + (fb.regs[4] & 15)),
        0};
    RtcTmToUnix(&tm, &t);
    RtcUnixToTm(t + 1, &tm);
    putTime(tm.year, tm.mon, tm.mday, tm.hour, tm.min, tm.sec, tm.wday,
            (fb.regs[4] & 0x80) != 0);
}

static int busRead(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {
    fake_bus_t *b = ctx;
    if (b->nTx < MAX_TX)
        b->log[b->nTx] = (tx_t){false, reg, len};
    b->nTx++;
    if (b->readRc < 0)
        return b->readRc;
    memcpy(buf, &b->regs[reg], len);
    if (b->rolloverAfter)
        tick();
    return 0;
}

static int busWrite(void *ctx, uint8_t reg, const uint8_t *buf, size_t len) {
    fake_bus_t *b = ctx;
    if (b->nTx < MAX_TX)
        b->log[b->nTx] = (tx_t){true, reg, len};
    b->nTx++;
    if (b->writeRc < 0)
        return b->writeRc;
    bool oldOs = (b->regs[4] & 0x80) != 0;
    memcpy(&b->regs[reg], buf, len);
    if (reg <= REG_SECONDS && reg + len > REG_SECONDS && b->oscDead && oldOs)
        b->regs[4] |= 0x80;
    return 0;
}

static rtc_bus_t bus(void) { return (rtc_bus_t){busRead, busWrite, &fb}; }

static void resetBus(void) {
    memset(&fb, 0, sizeof(fb));
    putTime(2026, 10, 6, 15, 20, 0, 2, false);
}

static void setupAll(void) {
    resetBus();
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
}

static void teardownAll(void) {
    RtcDevicesReset();
    RtcChipsReset();
}

static void expectTm(const rtc_tm_t *tm, unsigned y, unsigned mo, unsigned d,
                     unsigned h, unsigned mi, unsigned s, unsigned wd) {
    TEST_ASSERT_EQUAL_UINT(y, tm->year);
    TEST_ASSERT_EQUAL_UINT(mo, tm->mon);
    TEST_ASSERT_EQUAL_UINT(d, tm->mday);
    TEST_ASSERT_EQUAL_UINT(h, tm->hour);
    TEST_ASSERT_EQUAL_UINT(mi, tm->min);
    TEST_ASSERT_EQUAL_UINT(s, tm->sec);
    TEST_ASSERT_EQUAL_UINT(wd, tm->wday);
}

static void expectRegs(unsigned s, unsigned mi, unsigned h, unsigned d,
                       unsigned wd, unsigned mo, unsigned y) {
    TEST_ASSERT_EQUAL_HEX8(s, fb.regs[4]);
    TEST_ASSERT_EQUAL_HEX8(mi, fb.regs[5]);
    TEST_ASSERT_EQUAL_HEX8(h, fb.regs[6]);
    TEST_ASSERT_EQUAL_HEX8(d, fb.regs[7]);
    TEST_ASSERT_EQUAL_HEX8(wd, fb.regs[8]);
    TEST_ASSERT_EQUAL_HEX8(mo, fb.regs[9]);
    TEST_ASSERT_EQUAL_HEX8(y, fb.regs[10]);
}

/***************************************/
TEST_GROUP(rtc_pcf_get);
/***************************************/

TEST_SETUP(rtc_pcf_get) { setupAll(); }
TEST_TEAR_DOWN(rtc_pcf_get) { teardownAll(); }

TEST(rtc_pcf_get, DecodesTheTimeRegisters) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2026, 10, 6, 15, 20, 0, 2);
}

TEST(rtc_pcf_get, DecodesTheRangeEdges) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    putTime(2099, 12, 31, 23, 59, 59, 4, false);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2099, 12, 31, 23, 59, 59, 4);
    putTime(2000, 1, 1, 0, 0, 0, 6, false);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2000, 1, 1, 0, 0, 0, 6);
}

TEST(rtc_pcf_get, TheOscillatorStopFlagMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    putTime(2026, 10, 6, 15, 20, 0, 2, true);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf_get, ABadBcdDigitMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    for (int reg = 4; reg <= 10; reg++) {
        if (reg == 8)
            continue;
        resetBus();
        fb.regs[reg] = (uint8_t)((fb.regs[reg] & 0xF0) | 0x0A);
        bool valid = true;
        TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
        TEST_ASSERT_FALSE_MESSAGE(valid, "low nibble");
    }
    resetBus();
    fb.regs[10] = 0xA1;
    bool valid = true;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf_get, AWeekdayAboveSixMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    fb.regs[8] = 7;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf_get, UnusedBitsAreMaskedOff) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    fb.regs[5] |= 0x80;
    fb.regs[6] |= 0xC0;
    fb.regs[7] |= 0xC0;
    fb.regs[8] |= 0xF8;
    fb.regs[9] |= 0xE0;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2026, 10, 6, 15, 20, 0, 2);
}

TEST(rtc_pcf_get, ReadsEveryTimeRegisterInOneTransaction) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid;
    RtcChipPcf85063.get(&b, &tm, &valid);
    TEST_ASSERT_EQUAL_INT(1, fb.nTx);
    TEST_ASSERT_FALSE(fb.log[0].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[0].reg);
    TEST_ASSERT_EQUAL_UINT(7, fb.log[0].len);
}

TEST(rtc_pcf_get, ARolloverAfterTheReadCannotTearTheTime) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    putTime(2026, 12, 31, 23, 59, 59, 4, false);
    fb.rolloverAfter = true;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2026, 12, 31, 23, 59, 59, 4);
    TEST_ASSERT_EQUAL_INT(1, fb.nTx);
}

TEST(rtc_pcf_get, ABusErrorIsReturned) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    fb.readRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf85063.get(&b, &tm, &valid));
}

TEST_GROUP_RUNNER(rtc_pcf_get) {
    RUN_TEST_CASE(rtc_pcf_get, DecodesTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf_get, DecodesTheRangeEdges);
    RUN_TEST_CASE(rtc_pcf_get, TheOscillatorStopFlagMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf_get, ABadBcdDigitMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf_get, AWeekdayAboveSixMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf_get, UnusedBitsAreMaskedOff);
    RUN_TEST_CASE(rtc_pcf_get, ReadsEveryTimeRegisterInOneTransaction);
    RUN_TEST_CASE(rtc_pcf_get, ARolloverAfterTheReadCannotTearTheTime);
    RUN_TEST_CASE(rtc_pcf_get, ABusErrorIsReturned);
}

/***************************************/
TEST_GROUP(rtc_pcf_set);
/***************************************/

TEST_SETUP(rtc_pcf_set) { setupAll(); }
TEST_TEAR_DOWN(rtc_pcf_set) { teardownAll(); }

TEST(rtc_pcf_set, EncodesTheTimeRegisters) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    memset(fb.regs, 0, sizeof(fb.regs));
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &tm));
    expectRegs(0x00, 0x20, 0x15, 0x06, 2, 0x10, 0x26);
}

TEST(rtc_pcf_set, EncodesTheRangeEdgesAndTheLeapDays) {
    rtc_bus_t b = bus();
    rtc_tm_t last = {2099, 12, 31, 23, 59, 59, 4};
    rtc_tm_t first = {2000, 1, 1, 0, 0, 0, 6};
    rtc_tm_t leap96 = {2096, 2, 29, 12, 34, 56, 3};
    rtc_tm_t leap00 = {2000, 2, 29, 0, 0, 0, 2};
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &last));
    expectRegs(0x59, 0x59, 0x23, 0x31, 4, 0x12, 0x99);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &first));
    expectRegs(0x00, 0x00, 0x00, 0x01, 6, 0x01, 0x00);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &leap96));
    expectRegs(0x56, 0x34, 0x12, 0x29, 3, 0x02, 0x96);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &leap00));
    expectRegs(0x00, 0x00, 0x00, 0x29, 2, 0x02, 0x00);
}

TEST(rtc_pcf_set, WritesAllTimeRegistersInOneTransactionThenChecksTheFlag) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &tm));
    TEST_ASSERT_EQUAL_INT(2, fb.nTx);
    TEST_ASSERT_TRUE(fb.log[0].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[0].reg);
    TEST_ASSERT_EQUAL_UINT(7, fb.log[0].len);
    TEST_ASSERT_FALSE(fb.log[1].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[1].reg);
    TEST_ASSERT_EQUAL_UINT(1, fb.log[1].len);
}

TEST(rtc_pcf_set, TouchesOnlyTheTimeRegisters) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    for (int i = 0; i < REGS; i++)
        fb.regs[i] = (uint8_t)(0xA0 + i);
    fb.regs[4] |= 0x80;
    uint8_t before[REGS];
    memcpy(before, fb.regs, sizeof(before));
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &tm));
    for (int i = 0; i < REGS; i++) {
        if (i >= REG_SECONDS && i < REG_ALARM_FIRST)
            continue;
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], fb.regs[i], "outside time");
    }
}

TEST(rtc_pcf_set, ClearsTheOscillatorStopFlag) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.set(&b, &tm));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[4] & 0x80);
}

TEST(rtc_pcf_set, ADeadOscillatorIsEio) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    fb.oscDead = true;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcf85063.set(&b, &tm));
}

TEST(rtc_pcf_set, ABusErrorIsReturnedFromTheWriteAndFromTheCheck) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    fb.writeRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf85063.set(&b, &tm));
    fb.writeRc = 0;
    fb.readRc = -ENODEV;
    TEST_ASSERT_EQUAL_INT(-ENODEV, RtcChipPcf85063.set(&b, &tm));
}

TEST(rtc_pcf_set, ABadTimeIsRefusedWithoutBusTraffic) {
    rtc_bus_t b = bus();
    rtc_tm_t bad[] = {
        {2100, 1, 1, 0, 0, 0, 0},  {1999, 1, 1, 0, 0, 0, 0},
        {2026, 13, 1, 0, 0, 0, 0}, {2026, 1, 1, 0, 0, 0, 7},
        {2100, 2, 29, 0, 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipPcf85063.set(&b, &bad[i]));
    TEST_ASSERT_EQUAL_INT(0, fb.nTx);
}

TEST_GROUP_RUNNER(rtc_pcf_set) {
    RUN_TEST_CASE(rtc_pcf_set, EncodesTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf_set, EncodesTheRangeEdgesAndTheLeapDays);
    RUN_TEST_CASE(rtc_pcf_set,
                  WritesAllTimeRegistersInOneTransactionThenChecksTheFlag);
    RUN_TEST_CASE(rtc_pcf_set, TouchesOnlyTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf_set, ClearsTheOscillatorStopFlag);
    RUN_TEST_CASE(rtc_pcf_set, ADeadOscillatorIsEio);
    RUN_TEST_CASE(rtc_pcf_set, ABusErrorIsReturnedFromTheWriteAndFromTheCheck);
    RUN_TEST_CASE(rtc_pcf_set, ABadTimeIsRefusedWithoutBusTraffic);
}

/***************************************/
TEST_GROUP(rtc_pcf_init);
/***************************************/

TEST_SETUP(rtc_pcf_init) { setupAll(); }
TEST_TEAR_DOWN(rtc_pcf_init) { teardownAll(); }

TEST(rtc_pcf_init, ClearsStopAndTwelveHourModeAndKeepsTheRest) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x20 | 0x02 | 0x01 | 0x04;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0x01 | 0x04, fb.regs[REG_CONTROL1]);
}

TEST(rtc_pcf_init, ClearsEachBitOnItsOwn) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x20;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[REG_CONTROL1]);
    fb.regs[REG_CONTROL1] = 0x02;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[REG_CONTROL1]);
}

TEST(rtc_pcf_init, WritesNothingWhenTheSettingsAreRight) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x01;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf85063.init(&b));
    TEST_ASSERT_EQUAL_INT(1, fb.nTx);
    TEST_ASSERT_FALSE(fb.log[0].write);
    TEST_ASSERT_EQUAL_HEX8(0x01, fb.regs[REG_CONTROL1]);
}

TEST(rtc_pcf_init, NeverTouchesTheTimeRegisters) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x22;
    uint8_t before[7];
    memcpy(before, &fb.regs[4], 7);
    RtcChipPcf85063.init(&b);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, &fb.regs[4], 7);
}

TEST(rtc_pcf_init, ABusErrorIsReturned) {
    rtc_bus_t b = bus();
    fb.readRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcf85063.init(&b));
    fb.readRc = 0;
    fb.regs[REG_CONTROL1] = 0x20;
    fb.writeRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf85063.init(&b));
}

TEST_GROUP_RUNNER(rtc_pcf_init) {
    RUN_TEST_CASE(rtc_pcf_init, ClearsStopAndTwelveHourModeAndKeepsTheRest);
    RUN_TEST_CASE(rtc_pcf_init, ClearsEachBitOnItsOwn);
    RUN_TEST_CASE(rtc_pcf_init, WritesNothingWhenTheSettingsAreRight);
    RUN_TEST_CASE(rtc_pcf_init, NeverTouchesTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf_init, ABusErrorIsReturned);
}

/***************************************/
TEST_GROUP(rtc_chip_device);
/***************************************/

typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t owner;

static bool attach(const char *options) {
    owner.vfs = VfsInit();
    owner.drv = VfsRtcInit(NULL, options);
    if (owner.drv == NULL) {
        VfsDestroy(&owner.vfs);
        return false;
    }
    DevFs_Register(owner.vfs, "rtc", owner.drv);
    return true;
}

static void detach(void) {
    if (owner.drv != NULL)
        VfsDestroy(&owner.vfs);
    owner.drv = NULL;
}

static int readNode(const char *path, char *buf, size_t bufLen) {
    char full[64];
    snprintf(full, sizeof(full), "/dev/rtc/%s", path);
    int fd = VfsOpen(owner.vfs, full, VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(owner.vfs, fd, buf, bufLen - 1);
    VfsClose(owner.vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int writeTime(const char *payload) {
    int fd = VfsOpen(owner.vfs, "/dev/rtc/main/time", VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(owner.vfs, fd, payload, strlen(payload));
    VfsClose(owner.vfs, fd);
    return n;
}

static void expectLine(const char *path, const char *want) {
    char buf[32];
    TEST_ASSERT_EQUAL_INT((int)strlen(want), readNode(path, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING(want, buf);
}

static uint32_t sysSeconds(void) {
    plat_timestamp_t ns = 0;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &ns);
    return (uint32_t)(ns / NS_PER_S);
}

static void registerMain(void) {
    rtc_bus_t b = bus();
    TEST_ASSERT_EQUAL_INT(0, RtcChipRegister("main", &RtcChipPcf85063, &b));
}

TEST_SETUP(rtc_chip_device) { setupAll(); }
TEST_TEAR_DOWN(rtc_chip_device) {
    detach();
    teardownAll();
}

TEST(rtc_chip_device, RegisterRefusesAMissingPart) {
    rtc_bus_t b = bus();
    rtc_bus_t noRead = {NULL, busWrite, &fb};
    rtc_bus_t noWrite = {busRead, NULL, &fb};
    rtc_chip_t noInit = {NULL, RtcChipPcf85063.get, RtcChipPcf85063.set};
    rtc_chip_t noGet = {RtcChipPcf85063.init, NULL, RtcChipPcf85063.set};
    rtc_chip_t noSet = {RtcChipPcf85063.init, RtcChipPcf85063.get, NULL};
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipRegister(NULL, &RtcChipPcf85063, &b));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipRegister("main", NULL, &b));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          RtcChipRegister("main", &RtcChipPcf85063, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          RtcChipRegister("main", &RtcChipPcf85063, &noRead));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          RtcChipRegister("main", &RtcChipPcf85063, &noWrite));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipRegister("main", &noInit, &b));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipRegister("main", &noGet, &b));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipRegister("main", &noSet, &b));
    TEST_ASSERT_EQUAL_INT(0, fb.nTx);
}

TEST(rtc_chip_device, RegisterRunsTheChipInit) {
    fb.regs[REG_CONTROL1] = 0x22;
    registerMain();
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[REG_CONTROL1]);
}

TEST(rtc_chip_device, AFailedInitRegistersNothing) {
    rtc_bus_t b = bus();
    fb.readRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipRegister("aux", &RtcChipPcf85063, &b));
    fb.readRc = 0;
    TEST_ASSERT_NULL(VfsRtcInit(NULL, "devices=aux"));
    TEST_ASSERT_EQUAL_INT(0, RtcChipRegister("aux", &RtcChipPcf85063, &b));
}

TEST(rtc_chip_device, RegisterReportsNameErrorsAndKeepsItsPoolSlot) {
    rtc_bus_t b = bus();
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          RtcChipRegister("a b", &RtcChipPcf85063, &b));
    TEST_ASSERT_EQUAL_INT(0, RtcChipRegister("a", &RtcChipPcf85063, &b));
    TEST_ASSERT_EQUAL_INT(-EEXIST, RtcChipRegister("a", &RtcChipPcf85063, &b));
    TEST_ASSERT_EQUAL_INT(0, RtcChipRegister("b", &RtcChipPcf85063, &b));
    TEST_ASSERT_EQUAL_INT(-ENOSPC, RtcChipRegister("c", &RtcChipPcf85063, &b));
}

TEST(rtc_chip_device, TimeAndStatusReadTheChip) {
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main"));
    expectLine("main/time", "1791300000\n");
    expectLine("main/status", "valid\n");
}

TEST(rtc_chip_device, AStoppedOscillatorIsInvalid) {
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main"));
    fb.regs[4] |= 0x80;
    expectLine("main/status", "invalid\n");
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-EIO, readNode("main/time", buf, sizeof(buf)));
}

TEST(rtc_chip_device, RegistersThatMakeNoDateAreInvalid) {
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main"));
    fb.regs[9] = 0x13;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[9] = 0x00;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[7] = 0x32;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[7] = 0x00;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[6] = 0x24;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[5] = 0x60;
    expectLine("main/status", "invalid\n");
    resetBus();
    fb.regs[4] = 0x60;
    expectLine("main/status", "invalid\n");
    resetBus();
    putTime(2100, 2, 29, 0, 0, 0, 0, false);
    fb.regs[10] = 0x00;
    fb.regs[9] = 0x02;
    fb.regs[7] = 0x30;
    expectLine("main/status", "invalid\n");
}

TEST(rtc_chip_device, AChipThatDoesNotAnswerIsEio) {
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main"));
    fb.readRc = -ETIMEDOUT;
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-EIO, readNode("main/time", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EIO, readNode("main/status", buf, sizeof(buf)));
}

TEST(rtc_chip_device, BootTakesTheChipTimeAsTheSystemClock) {
    registerMain();
    TEST_ASSERT_TRUE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() - T_NOW < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach("devices=main"));
    expectLine("main/source", "rtc\n");
}

TEST(rtc_chip_device, BootIgnoresAChipThatLostPower) {
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    registerMain();
    TEST_ASSERT_FALSE(RtcBoot());
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
}

TEST(rtc_chip_device, AWriteSetsTheChipAndTheSystemClock) {
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main,set"));
    TEST_ASSERT_EQUAL_INT(15, writeTime("1800000000 sntp"));
    expectRegs(0x00, 0x00, 0x08, 0x15, 5, 0x01, 0x27);
    expectLine("main/status", "valid\n");
    expectLine("main/source", "sntp\n");
    TEST_ASSERT_TRUE(sysSeconds() - 1800000000UL < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SNTP_CALIBRATED,
                            WantedGetClockQuality());
}

TEST(rtc_chip_device, ADeadOscillatorFailsTheWriteAndKeepsTheClock) {
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    fb.oscDead = true;
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main,set"));
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime("1800000000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, WantedGetClockQuality());
    expectLine("main/source", "none\n");
}

TEST(rtc_chip_device, ABusErrorFailsTheWriteAndKeepsTheClock) {
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main,set"));
    fb.writeRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-EIO, writeTime("1800000000 sntp"));
    TEST_ASSERT_TRUE(sysSeconds() < 5);
}

TEST(rtc_chip_device, ATimeSetSurvivesAReset) {
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    registerMain();
    TEST_ASSERT_TRUE(attach("devices=main,set"));
    TEST_ASSERT_EQUAL_INT(15, writeTime("1800000000 sntp"));
    detach();
    RtcDevicesReset();
    RtcChipsReset();
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);

    registerMain();
    TEST_ASSERT_TRUE(RtcBoot());
    TEST_ASSERT_TRUE(sysSeconds() - 1800000000UL < 5);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC, WantedGetClockQuality());
    TEST_ASSERT_TRUE(attach("devices=main"));
    expectLine("main/source", "rtc\n");
}

TEST_GROUP_RUNNER(rtc_chip_device) {
    RUN_TEST_CASE(rtc_chip_device, RegisterRefusesAMissingPart);
    RUN_TEST_CASE(rtc_chip_device, RegisterRunsTheChipInit);
    RUN_TEST_CASE(rtc_chip_device, AFailedInitRegistersNothing);
    RUN_TEST_CASE(rtc_chip_device,
                  RegisterReportsNameErrorsAndKeepsItsPoolSlot);
    RUN_TEST_CASE(rtc_chip_device, TimeAndStatusReadTheChip);
    RUN_TEST_CASE(rtc_chip_device, AStoppedOscillatorIsInvalid);
    RUN_TEST_CASE(rtc_chip_device, RegistersThatMakeNoDateAreInvalid);
    RUN_TEST_CASE(rtc_chip_device, AChipThatDoesNotAnswerIsEio);
    RUN_TEST_CASE(rtc_chip_device, BootTakesTheChipTimeAsTheSystemClock);
    RUN_TEST_CASE(rtc_chip_device, BootIgnoresAChipThatLostPower);
    RUN_TEST_CASE(rtc_chip_device, AWriteSetsTheChipAndTheSystemClock);
    RUN_TEST_CASE(rtc_chip_device,
                  ADeadOscillatorFailsTheWriteAndKeepsTheClock);
    RUN_TEST_CASE(rtc_chip_device, ABusErrorFailsTheWriteAndKeepsTheClock);
    RUN_TEST_CASE(rtc_chip_device, ATimeSetSurvivesAReset);
}
