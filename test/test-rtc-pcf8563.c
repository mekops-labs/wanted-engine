/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <rtc-chip.h>
#include <rtc-time.h>

/* The PCF8563 driver and the PCF85063A/PCF8563 probe over a fake register
 * bus. The fake keeps the VL flag set while the oscillator is dead, and drops
 * bit 7 of Minutes, which the PCF8563 lacks. */

#define REG_CONTROL1 0x00
#define REG_SECONDS 0x02
#define REG_MINUTES 0x03
#define REG_ALARM_FIRST 0x09
#define REGS 0x10
#define MAX_TX 16

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
    int failWriteAt; /* 1-based transaction number to fail, 0 for none */
    bool oscDead;
    bool pcf85063; /* register 03h keeps all eight bits */
} fake_bus_t;

static fake_bus_t fb;

static uint8_t bcd(unsigned v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void putTime(unsigned y, unsigned mo, unsigned d, unsigned h,
                    unsigned mi, unsigned s, unsigned wd, bool vl) {
    fb.regs[2] = (uint8_t)(bcd(s) | (vl ? 0x80 : 0));
    fb.regs[3] = bcd(mi);
    fb.regs[4] = bcd(h);
    fb.regs[5] = bcd(d);
    fb.regs[6] = (uint8_t)wd;
    fb.regs[7] = bcd(mo);
    fb.regs[8] = bcd(y - 2000);
}

static int busRead(void *ctx, uint8_t reg, uint8_t *buf, size_t len) {
    fake_bus_t *b = ctx;
    if (b->nTx < MAX_TX)
        b->log[b->nTx] = (tx_t){false, reg, len};
    b->nTx++;
    if (b->readRc < 0)
        return b->readRc;
    memcpy(buf, &b->regs[reg], len);
    return 0;
}

static int busWrite(void *ctx, uint8_t reg, const uint8_t *buf, size_t len) {
    fake_bus_t *b = ctx;
    if (b->nTx < MAX_TX)
        b->log[b->nTx] = (tx_t){true, reg, len};
    b->nTx++;
    if (b->writeRc < 0)
        return b->writeRc;
    if (b->failWriteAt == b->nTx)
        return -EIO;
    bool oldVl = (b->regs[2] & 0x80) != 0;
    memcpy(&b->regs[reg], buf, len);
    if (reg <= REG_SECONDS && reg + len > REG_SECONDS && b->oscDead && oldVl)
        b->regs[2] |= 0x80;
    if (!b->pcf85063 && reg <= REG_MINUTES && reg + len > REG_MINUTES)
        b->regs[3] &= 0x7F;
    return 0;
}

static rtc_bus_t bus(void) { return (rtc_bus_t){busRead, busWrite, &fb}; }

static void resetBus(void) {
    memset(&fb, 0, sizeof(fb));
    putTime(2026, 10, 6, 15, 20, 0, 2, false);
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
    TEST_ASSERT_EQUAL_HEX8(s, fb.regs[2]);
    TEST_ASSERT_EQUAL_HEX8(mi, fb.regs[3]);
    TEST_ASSERT_EQUAL_HEX8(h, fb.regs[4]);
    TEST_ASSERT_EQUAL_HEX8(d, fb.regs[5]);
    TEST_ASSERT_EQUAL_HEX8(wd, fb.regs[6]);
    TEST_ASSERT_EQUAL_HEX8(mo, fb.regs[7]);
    TEST_ASSERT_EQUAL_HEX8(y, fb.regs[8]);
}

/***************************************/
TEST_GROUP(rtc_pcf8563_get);
/***************************************/

TEST_SETUP(rtc_pcf8563_get) { resetBus(); }
TEST_TEAR_DOWN(rtc_pcf8563_get) {}

TEST(rtc_pcf8563_get, DecodesTheTimeRegisters) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2026, 10, 6, 15, 20, 0, 2);
}

TEST(rtc_pcf8563_get, DecodesTheRangeEdges) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    putTime(2099, 12, 31, 23, 59, 59, 4, false);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2099, 12, 31, 23, 59, 59, 4);
    putTime(2000, 1, 1, 0, 0, 0, 6, false);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2000, 1, 1, 0, 0, 0, 6);
}

TEST(rtc_pcf8563_get, TheVoltageLowFlagMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    putTime(2026, 10, 6, 15, 20, 0, 2, true);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf8563_get, TheCenturyBitMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    fb.regs[7] |= 0x80;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf8563_get, ABadBcdDigitMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    for (int reg = 2; reg <= 8; reg++) {
        if (reg == 6)
            continue;
        resetBus();
        fb.regs[reg] = (uint8_t)((fb.regs[reg] & 0xF0) | 0x0A);
        bool valid = true;
        TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
        TEST_ASSERT_FALSE_MESSAGE(valid, "low nibble");
    }
    resetBus();
    fb.regs[8] = 0xA1;
    bool valid = true;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf8563_get, AWeekdayAboveSixMakesItInvalid) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    fb.regs[6] = 7;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_FALSE(valid);
}

TEST(rtc_pcf8563_get, UnusedBitsAreMaskedOff) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = false;
    fb.regs[3] |= 0x80;
    fb.regs[4] |= 0xC0;
    fb.regs[5] |= 0xC0;
    fb.regs[6] |= 0xF8;
    fb.regs[7] |= 0x60;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.get(&b, &tm, &valid));
    TEST_ASSERT_TRUE(valid);
    expectTm(&tm, 2026, 10, 6, 15, 20, 0, 2);
}

TEST(rtc_pcf8563_get, ReadsEveryTimeRegisterInOneTransaction) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid;
    RtcChipPcf8563.get(&b, &tm, &valid);
    TEST_ASSERT_EQUAL_INT(1, fb.nTx);
    TEST_ASSERT_FALSE(fb.log[0].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[0].reg);
    TEST_ASSERT_EQUAL_UINT(7, fb.log[0].len);
}

TEST(rtc_pcf8563_get, ABusErrorIsReturned) {
    rtc_bus_t b = bus();
    rtc_tm_t tm;
    bool valid = true;
    fb.readRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf8563.get(&b, &tm, &valid));
}

TEST_GROUP_RUNNER(rtc_pcf8563_get) {
    RUN_TEST_CASE(rtc_pcf8563_get, DecodesTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf8563_get, DecodesTheRangeEdges);
    RUN_TEST_CASE(rtc_pcf8563_get, TheVoltageLowFlagMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf8563_get, TheCenturyBitMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf8563_get, ABadBcdDigitMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf8563_get, AWeekdayAboveSixMakesItInvalid);
    RUN_TEST_CASE(rtc_pcf8563_get, UnusedBitsAreMaskedOff);
    RUN_TEST_CASE(rtc_pcf8563_get, ReadsEveryTimeRegisterInOneTransaction);
    RUN_TEST_CASE(rtc_pcf8563_get, ABusErrorIsReturned);
}

/***************************************/
TEST_GROUP(rtc_pcf8563_set);
/***************************************/

TEST_SETUP(rtc_pcf8563_set) { resetBus(); }
TEST_TEAR_DOWN(rtc_pcf8563_set) {}

TEST(rtc_pcf8563_set, EncodesTheTimeRegistersWithTheCenturyBitClear) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    memset(fb.regs, 0, sizeof(fb.regs));
    fb.regs[7] = 0x80;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &tm));
    expectRegs(0x00, 0x20, 0x15, 0x06, 2, 0x10, 0x26);
}

TEST(rtc_pcf8563_set, EncodesTheRangeEdgesAndTheLeapDays) {
    rtc_bus_t b = bus();
    rtc_tm_t last = {2099, 12, 31, 23, 59, 59, 4};
    rtc_tm_t first = {2000, 1, 1, 0, 0, 0, 6};
    rtc_tm_t leap96 = {2096, 2, 29, 12, 34, 56, 3};
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &last));
    expectRegs(0x59, 0x59, 0x23, 0x31, 4, 0x12, 0x99);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &first));
    expectRegs(0x00, 0x00, 0x00, 0x01, 6, 0x01, 0x00);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &leap96));
    expectRegs(0x56, 0x34, 0x12, 0x29, 3, 0x02, 0x96);
}

TEST(rtc_pcf8563_set, WritesAllTimeRegistersInOneTransactionThenChecksVl) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &tm));
    TEST_ASSERT_EQUAL_INT(2, fb.nTx);
    TEST_ASSERT_TRUE(fb.log[0].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[0].reg);
    TEST_ASSERT_EQUAL_UINT(7, fb.log[0].len);
    TEST_ASSERT_FALSE(fb.log[1].write);
    TEST_ASSERT_EQUAL_UINT8(REG_SECONDS, fb.log[1].reg);
    TEST_ASSERT_EQUAL_UINT(1, fb.log[1].len);
}

TEST(rtc_pcf8563_set, TouchesOnlyTheTimeRegisters) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    for (int i = 0; i < REGS; i++)
        fb.regs[i] = (uint8_t)(0xA0 + i);
    uint8_t before[REGS];
    memcpy(before, fb.regs, sizeof(before));
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &tm));
    for (int i = 0; i < REGS; i++) {
        if (i >= REG_SECONDS && i < REG_ALARM_FIRST)
            continue;
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(before[i], fb.regs[i], "outside time");
    }
}

TEST(rtc_pcf8563_set, ClearsTheVoltageLowFlag) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.set(&b, &tm));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[2] & 0x80);
}

TEST(rtc_pcf8563_set, ADeadOscillatorIsEio) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    putTime(2000, 1, 1, 0, 0, 0, 6, true);
    fb.oscDead = true;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcf8563.set(&b, &tm));
}

TEST(rtc_pcf8563_set, ABusErrorIsReturnedFromTheWriteAndFromTheCheck) {
    rtc_bus_t b = bus();
    rtc_tm_t tm = {2026, 10, 6, 15, 20, 0, 2};
    fb.writeRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf8563.set(&b, &tm));
    fb.writeRc = 0;
    fb.readRc = -ENODEV;
    TEST_ASSERT_EQUAL_INT(-ENODEV, RtcChipPcf8563.set(&b, &tm));
}

TEST(rtc_pcf8563_set, ABadTimeIsRefusedWithoutBusTraffic) {
    rtc_bus_t b = bus();
    rtc_tm_t bad[] = {
        {2100, 1, 1, 0, 0, 0, 0},  {1999, 1, 1, 0, 0, 0, 0},
        {2026, 13, 1, 0, 0, 0, 0}, {2026, 1, 1, 0, 0, 0, 7},
        {2100, 2, 29, 0, 0, 0, 0},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_INT(-EINVAL, RtcChipPcf8563.set(&b, &bad[i]));
    TEST_ASSERT_EQUAL_INT(0, fb.nTx);
}

TEST_GROUP_RUNNER(rtc_pcf8563_set) {
    RUN_TEST_CASE(rtc_pcf8563_set,
                  EncodesTheTimeRegistersWithTheCenturyBitClear);
    RUN_TEST_CASE(rtc_pcf8563_set, EncodesTheRangeEdgesAndTheLeapDays);
    RUN_TEST_CASE(rtc_pcf8563_set,
                  WritesAllTimeRegistersInOneTransactionThenChecksVl);
    RUN_TEST_CASE(rtc_pcf8563_set, TouchesOnlyTheTimeRegisters);
    RUN_TEST_CASE(rtc_pcf8563_set, ClearsTheVoltageLowFlag);
    RUN_TEST_CASE(rtc_pcf8563_set, ADeadOscillatorIsEio);
    RUN_TEST_CASE(rtc_pcf8563_set,
                  ABusErrorIsReturnedFromTheWriteAndFromTheCheck);
    RUN_TEST_CASE(rtc_pcf8563_set, ABadTimeIsRefusedWithoutBusTraffic);
}

/***************************************/
TEST_GROUP(rtc_pcf8563_init);
/***************************************/

TEST_SETUP(rtc_pcf8563_init) { resetBus(); }
TEST_TEAR_DOWN(rtc_pcf8563_init) {}

TEST(rtc_pcf8563_init, ClearsStopAndTest1AndKeepsTestc) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x80 | 0x20 | 0x08;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0x08, fb.regs[REG_CONTROL1]);
}

TEST(rtc_pcf8563_init, ClearsEachBitOnItsOwn) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x20;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[REG_CONTROL1]);
    fb.regs[REG_CONTROL1] = 0x80;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.init(&b));
    TEST_ASSERT_EQUAL_HEX8(0, fb.regs[REG_CONTROL1]);
}

TEST(rtc_pcf8563_init, TheWriteIsOneByteAtControlStatus1) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x20;
    fb.regs[1] = 0x1F;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.init(&b));
    TEST_ASSERT_EQUAL_INT(2, fb.nTx);
    TEST_ASSERT_TRUE(fb.log[1].write);
    TEST_ASSERT_EQUAL_UINT8(REG_CONTROL1, fb.log[1].reg);
    TEST_ASSERT_EQUAL_UINT(1, fb.log[1].len);
    TEST_ASSERT_EQUAL_HEX8(0x1F, fb.regs[1]);
}

TEST(rtc_pcf8563_init, WritesNothingWhenTheSettingsAreRight) {
    rtc_bus_t b = bus();
    fb.regs[REG_CONTROL1] = 0x08;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcf8563.init(&b));
    TEST_ASSERT_EQUAL_INT(1, fb.nTx);
    TEST_ASSERT_FALSE(fb.log[0].write);
}

TEST(rtc_pcf8563_init, ABusErrorIsReturned) {
    rtc_bus_t b = bus();
    fb.readRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcf8563.init(&b));
    fb.readRc = 0;
    fb.regs[REG_CONTROL1] = 0x20;
    fb.writeRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcf8563.init(&b));
}

TEST_GROUP_RUNNER(rtc_pcf8563_init) {
    RUN_TEST_CASE(rtc_pcf8563_init, ClearsStopAndTest1AndKeepsTestc);
    RUN_TEST_CASE(rtc_pcf8563_init, ClearsEachBitOnItsOwn);
    RUN_TEST_CASE(rtc_pcf8563_init, TheWriteIsOneByteAtControlStatus1);
    RUN_TEST_CASE(rtc_pcf8563_init, WritesNothingWhenTheSettingsAreRight);
    RUN_TEST_CASE(rtc_pcf8563_init, ABusErrorIsReturned);
}

/***************************************/
TEST_GROUP(rtc_pcf_detect);
/***************************************/

TEST_SETUP(rtc_pcf_detect) { resetBus(); }
TEST_TEAR_DOWN(rtc_pcf_detect) {}

TEST(rtc_pcf_detect, APcf8563DropsTheBitAndKeepsItsMinutes) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_EQUAL_PTR(&RtcChipPcf8563, chip);
    TEST_ASSERT_EQUAL_HEX8(0x20, fb.regs[REG_MINUTES]);
    TEST_ASSERT_EQUAL_INT(3, fb.nTx);
}

TEST(rtc_pcf_detect, APcf85063KeepsTheBitAndGetsItsRamByteBack) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    fb.pcf85063 = true;
    fb.regs[REG_MINUTES] = 0x5A;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_EQUAL_PTR(&RtcChipPcf85063, chip);
    TEST_ASSERT_EQUAL_HEX8(0x5A, fb.regs[REG_MINUTES]);
    TEST_ASSERT_EQUAL_INT(4, fb.nTx);
}

TEST(rtc_pcf_detect, ARamByteWithTheBitSetNeedsNoRestore) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    fb.pcf85063 = true;
    fb.regs[REG_MINUTES] = 0xC3;
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_EQUAL_PTR(&RtcChipPcf85063, chip);
    TEST_ASSERT_EQUAL_HEX8(0xC3, fb.regs[REG_MINUTES]);
    TEST_ASSERT_EQUAL_INT(3, fb.nTx);
}

TEST(rtc_pcf_detect, TouchesOnlyRegister03) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    fb.pcf85063 = true;
    uint8_t before[REGS];
    memcpy(before, fb.regs, sizeof(before));
    TEST_ASSERT_EQUAL_INT(0, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(before, fb.regs, REGS);
    for (int i = 0; i < fb.nTx; i++) {
        TEST_ASSERT_EQUAL_UINT8(REG_MINUTES, fb.log[i].reg);
        TEST_ASSERT_EQUAL_UINT(1, fb.log[i].len);
    }
}

TEST(rtc_pcf_detect, ABusErrorIsReturnedAndNoChipIsSet) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    fb.readRc = -ETIMEDOUT;
    TEST_ASSERT_EQUAL_INT(-ETIMEDOUT, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_NULL(chip);
    fb.readRc = 0;
    fb.writeRc = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcfDetect(&b, &chip));
    TEST_ASSERT_NULL(chip);
}

TEST(rtc_pcf_detect, AFailedRestoreIsReturned) {
    rtc_bus_t b = bus();
    const rtc_chip_t *chip = NULL;
    fb.pcf85063 = true;
    fb.failWriteAt = 4;
    TEST_ASSERT_EQUAL_INT(-EIO, RtcChipPcfDetect(&b, &chip));
}

TEST_GROUP_RUNNER(rtc_pcf_detect) {
    RUN_TEST_CASE(rtc_pcf_detect, APcf8563DropsTheBitAndKeepsItsMinutes);
    RUN_TEST_CASE(rtc_pcf_detect, APcf85063KeepsTheBitAndGetsItsRamByteBack);
    RUN_TEST_CASE(rtc_pcf_detect, ARamByteWithTheBitSetNeedsNoRestore);
    RUN_TEST_CASE(rtc_pcf_detect, TouchesOnlyRegister03);
    RUN_TEST_CASE(rtc_pcf_detect, ABusErrorIsReturnedAndNoChipIsSet);
    RUN_TEST_CASE(rtc_pcf_detect, AFailedRestoreIsReturned);
}
