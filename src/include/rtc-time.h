/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/* Seconds since 1970-01-01 UTC that a two-digit BCD year can hold:
 * 2000-01-01 00:00:00 to 2099-12-31 23:59:59. */
#define RTC_UNIX_MIN 946684800UL
#define RTC_UNIX_MAX 4102444799UL

/* A broken-down UTC time. `mon` is 1..12 and `wday` is 0 for Sunday. */
typedef struct rtc_tm_t {
    uint16_t year; /* 2000..2099 */
    uint8_t mon;
    uint8_t mday;
    uint8_t hour;
    uint8_t min;
    uint8_t sec;
    uint8_t wday;
} rtc_tm_t;

/* Unix time to calendar time. -EINVAL outside RTC_UNIX_MIN..RTC_UNIX_MAX. */
int RtcUnixToTm(uint32_t t, rtc_tm_t *tm);

/* Calendar time to Unix time. `wday` is ignored. -EINVAL for a field out of
 * range, a day the month lacks (there is no 2100-02-29), or a year outside
 * 2000..2099. */
int RtcTmToUnix(const rtc_tm_t *tm, uint32_t *t);

typedef enum rtc_source_t {
    RTC_SRC_NONE,
    RTC_SRC_RTC,
    RTC_SRC_SNTP,
    RTC_SRC_SERVER,
    RTC_SRC_MANUAL,
} rtc_source_t;

/* "none", "rtc", "sntp", "server" or "manual". */
const char *RtcSourceName(rtc_source_t src);

/* The /proc/clock_quality value a write with `src` leaves. */
uint8_t RtcSourceQuality(rtc_source_t src);

/* Parse one write to `time`: `<seconds>` or `<seconds> <source>`, with an
 * optional final newline. The source defaults to manual and is never none.
 * -EINVAL for anything else. */
int RtcParseWrite(const char *buf, size_t len, uint32_t *sec,
                  rtc_source_t *src);
