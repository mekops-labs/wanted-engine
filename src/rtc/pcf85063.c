/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <rtc-chip.h>

/* PCF85063A: registers 04h..0Ah hold the BCD time; bit 7 of Seconds (OS)
 * flags a stopped oscillator. The counters freeze during an access that must
 * end within a second, so each direction is one transaction. */

#define PCF_REG_CONTROL1 0x00U
#define PCF_REG_SECONDS 0x04U
#define PCF_TIME_REGS 7U

#define PCF_CONTROL1_STOP 0x20U
#define PCF_CONTROL1_12_24 0x02U
#define PCF_CONTROL1_FIXED (PCF_CONTROL1_STOP | PCF_CONTROL1_12_24)

#define PCF_SECONDS_OS 0x80U
#define PCF_SECONDS_MASK 0x7FU
#define PCF_MINUTES_MASK 0x7FU
#define PCF_HOURS_MASK 0x3FU
#define PCF_DAYS_MASK 0x3FU
#define PCF_WEEKDAY_MASK 0x07U
#define PCF_MONTHS_MASK 0x1FU
#define PCF_WEEKDAY_MAX 6U

#define PCF_YEAR_BASE 2000U
#define PCF_BCD_BITS 4U
#define PCF_BCD_DIGIT_MASK 0x0FU
#define PCF_BCD_DIGIT_MAX 9U
#define PCF_DECIMAL 10U

enum { SEC, MIN, HOUR, DAY, WDAY, MON, YEAR };

static uint8_t toBcd(unsigned v) {
    return (uint8_t)(((v / PCF_DECIMAL) << PCF_BCD_BITS) | (v % PCF_DECIMAL));
}

/* -1 when either nibble is not a decimal digit. */
static int fromBcd(uint8_t b) {
    unsigned tens = b >> PCF_BCD_BITS;
    unsigned ones = b & PCF_BCD_DIGIT_MASK;
    if (tens > PCF_BCD_DIGIT_MAX || ones > PCF_BCD_DIGIT_MAX)
        return -1;
    return (int)((tens * PCF_DECIMAL) + ones);
}

static int pcfInit(const rtc_bus_t *bus) {
    uint8_t c1 = 0;
    int rc = bus->read(bus->ctx, PCF_REG_CONTROL1, &c1, 1);
    if (rc < 0)
        return rc;
    if ((c1 & PCF_CONTROL1_FIXED) == 0)
        return 0;
    c1 &= (uint8_t)~PCF_CONTROL1_FIXED;
    return bus->write(bus->ctx, PCF_REG_CONTROL1, &c1, 1);
}

static int pcfGet(const rtc_bus_t *bus, rtc_tm_t *tm, bool *valid) {
    uint8_t r[PCF_TIME_REGS];
    int rc = bus->read(bus->ctx, PCF_REG_SECONDS, r, sizeof(r));
    if (rc < 0)
        return rc;

    int f[PCF_TIME_REGS];
    f[SEC] = fromBcd(r[SEC] & PCF_SECONDS_MASK);
    f[MIN] = fromBcd(r[MIN] & PCF_MINUTES_MASK);
    f[HOUR] = fromBcd(r[HOUR] & PCF_HOURS_MASK);
    f[DAY] = fromBcd(r[DAY] & PCF_DAYS_MASK);
    f[WDAY] = r[WDAY] & PCF_WEEKDAY_MASK;
    f[MON] = fromBcd(r[MON] & PCF_MONTHS_MASK);
    f[YEAR] = fromBcd(r[YEAR]);

    bool ok = (r[SEC] & PCF_SECONDS_OS) == 0 && f[WDAY] <= (int)PCF_WEEKDAY_MAX;
    for (unsigned i = 0; i < PCF_TIME_REGS; i++)
        ok = ok && f[i] >= 0;
    *valid = ok;
    if (!ok)
        return 0;

    tm->year = (uint16_t)(PCF_YEAR_BASE + (unsigned)f[YEAR]);
    tm->mon = (uint8_t)f[MON];
    tm->mday = (uint8_t)f[DAY];
    tm->hour = (uint8_t)f[HOUR];
    tm->min = (uint8_t)f[MIN];
    tm->sec = (uint8_t)f[SEC];
    tm->wday = (uint8_t)f[WDAY];
    return 0;
}

static int pcfSet(const rtc_bus_t *bus, const rtc_tm_t *tm) {
    uint32_t unused;
    if (RtcTmToUnix(tm, &unused) < 0 || tm->wday > PCF_WEEKDAY_MAX)
        return -EINVAL;

    uint8_t r[PCF_TIME_REGS];
    r[SEC] = toBcd(tm->sec);
    r[MIN] = toBcd(tm->min);
    r[HOUR] = toBcd(tm->hour);
    r[DAY] = toBcd(tm->mday);
    r[WDAY] = tm->wday;
    r[MON] = toBcd(tm->mon);
    r[YEAR] = toBcd((unsigned)tm->year - PCF_YEAR_BASE);
    int rc = bus->write(bus->ctx, PCF_REG_SECONDS, r, sizeof(r));
    if (rc < 0)
        return rc;

    /* The OS flag stays set while the oscillator is not running. */
    uint8_t sec = 0;
    rc = bus->read(bus->ctx, PCF_REG_SECONDS, &sec, 1);
    if (rc < 0)
        return rc;
    return (sec & PCF_SECONDS_OS) != 0 ? -EIO : 0;
}

const rtc_chip_t RtcChipPcf85063 = {pcfInit, pcfGet, pcfSet};
