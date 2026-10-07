/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <rtc-time.h>
#include <wanted-vfs-api.h>

#define RTC_EPOCH_YEAR 2000
#define RTC_LAST_YEAR 2099
#define RTC_SECS_PER_DAY 86400UL
#define RTC_SECS_PER_HOUR 3600UL
#define RTC_SECS_PER_MIN 60UL
#define RTC_MINS_PER_HOUR 60U
#define RTC_HOURS_PER_DAY 24U
#define RTC_DAYS_COMMON_YEAR 365UL
#define RTC_DAYS_LEAP_YEAR 366UL
#define RTC_DAYS_PER_CYCLE 1461UL /* four years, the first one leap */
#define RTC_YEARS_PER_CYCLE 4U
#define RTC_DAYS_PER_WEEK 7UL
#define RTC_EPOCH_WDAY 6U /* 2000-01-01 was a Saturday */
#define RTC_MONTHS 12U
#define RTC_FEB 2U
#define RTC_DIGITS_MAX 10U

static const uint8_t monthDays[RTC_MONTHS] = {31, 28, 31, 30, 31, 30,
                                              31, 31, 30, 31, 30, 31};

/* Within 2000..2099 every fourth year is a leap year, 2000 included. */
static bool isLeap(unsigned year) { return year % RTC_YEARS_PER_CYCLE == 0; }

static unsigned daysInMonth(unsigned year, unsigned mon) {
    unsigned d = monthDays[mon - 1U];
    return (mon == RTC_FEB && isLeap(year)) ? d + 1U : d;
}

int RtcUnixToTm(uint32_t t, rtc_tm_t *tm) {
    if (tm == NULL || t < RTC_UNIX_MIN || t > RTC_UNIX_MAX)
        return -EINVAL;

    uint32_t secs = t - (uint32_t)RTC_UNIX_MIN;
    uint32_t days = secs / RTC_SECS_PER_DAY;
    uint32_t rem = secs % RTC_SECS_PER_DAY;

    unsigned year =
        RTC_EPOCH_YEAR + (RTC_YEARS_PER_CYCLE * (days / RTC_DAYS_PER_CYCLE));
    uint32_t doy = days % RTC_DAYS_PER_CYCLE;
    if (doy >= RTC_DAYS_LEAP_YEAR) {
        doy -= RTC_DAYS_LEAP_YEAR;
        year += 1U + (unsigned)(doy / RTC_DAYS_COMMON_YEAR);
        doy %= RTC_DAYS_COMMON_YEAR;
    }

    unsigned mon = 1;
    while (doy >= daysInMonth(year, mon)) {
        doy -= daysInMonth(year, mon);
        mon++;
    }

    tm->year = (uint16_t)year;
    tm->mon = (uint8_t)mon;
    tm->mday = (uint8_t)(doy + 1U);
    tm->hour = (uint8_t)(rem / RTC_SECS_PER_HOUR);
    tm->min = (uint8_t)(rem % RTC_SECS_PER_HOUR / RTC_SECS_PER_MIN);
    tm->sec = (uint8_t)(rem % RTC_SECS_PER_MIN);
    tm->wday = (uint8_t)((RTC_EPOCH_WDAY + days) % RTC_DAYS_PER_WEEK);
    return 0;
}

int RtcTmToUnix(const rtc_tm_t *tm, uint32_t *t) {
    if (tm == NULL || t == NULL || tm->year < RTC_EPOCH_YEAR ||
        tm->year > RTC_LAST_YEAR || tm->mon < 1U || tm->mon > RTC_MONTHS ||
        tm->mday < 1U || tm->mday > daysInMonth(tm->year, tm->mon) ||
        tm->hour >= RTC_HOURS_PER_DAY || tm->min >= RTC_MINS_PER_HOUR ||
        tm->sec >= RTC_SECS_PER_MIN)
        return -EINVAL;

    uint32_t years = (uint32_t)(tm->year - RTC_EPOCH_YEAR);
    uint32_t days = (years * RTC_DAYS_COMMON_YEAR) +
                    ((years + RTC_YEARS_PER_CYCLE - 1U) / RTC_YEARS_PER_CYCLE);
    for (unsigned m = 1; m < tm->mon; m++)
        days += daysInMonth(tm->year, m);
    days += (uint32_t)(tm->mday - 1U);

    *t = (uint32_t)RTC_UNIX_MIN + (days * (uint32_t)RTC_SECS_PER_DAY) +
         (tm->hour * (uint32_t)RTC_SECS_PER_HOUR) +
         (tm->min * (uint32_t)RTC_SECS_PER_MIN) + tm->sec;
    return 0;
}

static const struct {
    const char *name;
    uint8_t quality;
} sources[] = {
    [RTC_SRC_NONE] = {"none", WANTED_CLOCK_UNCALIBRATED},
    [RTC_SRC_RTC] = {"rtc", WANTED_CLOCK_HARDWARE_RTC},
    [RTC_SRC_SNTP] = {"sntp", WANTED_CLOCK_SNTP_CALIBRATED},
    [RTC_SRC_SERVER] = {"server", WANTED_CLOCK_SIMPLE_CALIBRATION},
    [RTC_SRC_MANUAL] = {"manual", WANTED_CLOCK_SIMPLE_CALIBRATION},
};
#define RTC_SOURCE_COUNT (sizeof(sources) / sizeof(sources[0]))

const char *RtcSourceName(rtc_source_t src) {
    return (unsigned)src < RTC_SOURCE_COUNT ? sources[src].name : "none";
}

uint8_t RtcSourceQuality(rtc_source_t src) {
    return (unsigned)src < RTC_SOURCE_COUNT ? sources[src].quality
                                            : WANTED_CLOCK_UNCALIBRATED;
}

/* A source a writer may name: every one but none. */
static int findSource(const char *word, size_t len, rtc_source_t *src) {
    for (size_t i = RTC_SRC_RTC; i < RTC_SOURCE_COUNT; i++) {
        if (strlen(sources[i].name) == len &&
            memcmp(sources[i].name, word, len) == 0) {
            *src = (rtc_source_t)i;
            return 0;
        }
    }
    return -EINVAL;
}

int RtcParseWrite(const char *buf, size_t len, uint32_t *sec,
                  rtc_source_t *src) {
    if (buf == NULL || sec == NULL || src == NULL)
        return -EINVAL;
    if (len > 0 && buf[len - 1] == '\n')
        len--;

    size_t n = 0;
    uint64_t v = 0;
    while (n < len && buf[n] >= '0' && buf[n] <= '9') {
        if (n == RTC_DIGITS_MAX)
            return -EINVAL;
        v = v * 10U + (uint64_t)(buf[n] - '0');
        n++;
    }
    if (n == 0 || v < RTC_UNIX_MIN || v > RTC_UNIX_MAX)
        return -EINVAL;

    rtc_source_t found = RTC_SRC_MANUAL;
    if (n < len) {
        if (buf[n] != ' ' || findSource(buf + n + 1, len - n - 1, &found) < 0)
            return -EINVAL;
    }
    *sec = (uint32_t)v;
    *src = found;
    return 0;
}
