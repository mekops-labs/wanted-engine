/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include <rtc-time.h>
#include <wanted-vfs-api.h>

/* The pure parts of /dev/rtc: calendar conversion, the write grammar and the
 * source-to-quality table. */

static void expectTm(const rtc_tm_t *tm, unsigned year, unsigned mon,
                     unsigned mday, unsigned hour, unsigned min, unsigned sec,
                     unsigned wday) {
    TEST_ASSERT_EQUAL_UINT(year, tm->year);
    TEST_ASSERT_EQUAL_UINT(mon, tm->mon);
    TEST_ASSERT_EQUAL_UINT(mday, tm->mday);
    TEST_ASSERT_EQUAL_UINT(hour, tm->hour);
    TEST_ASSERT_EQUAL_UINT(min, tm->min);
    TEST_ASSERT_EQUAL_UINT(sec, tm->sec);
    TEST_ASSERT_EQUAL_UINT(wday, tm->wday);
}

static uint32_t unixOf(unsigned year, unsigned mon, unsigned mday,
                       unsigned hour, unsigned min, unsigned sec) {
    rtc_tm_t tm = {(uint16_t)year,
                   (uint8_t)mon,
                   (uint8_t)mday,
                   (uint8_t)hour,
                   (uint8_t)min,
                   (uint8_t)sec,
                   0};
    uint32_t t = 0;
    TEST_ASSERT_EQUAL_INT(0, RtcTmToUnix(&tm, &t));
    return t;
}

/***************************************/
TEST_GROUP(rtc_calendar);
/***************************************/

TEST_SETUP(rtc_calendar) {}
TEST_TEAR_DOWN(rtc_calendar) {}

TEST(rtc_calendar, RangeStartIsTheYear2000) {
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(946684800UL, &tm));
    expectTm(&tm, 2000, 1, 1, 0, 0, 0, 6);
}

TEST(rtc_calendar, RangeEndIsTheLastSecondOf2099) {
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(4102444799UL, &tm));
    expectTm(&tm, 2099, 12, 31, 23, 59, 59, 4);
}

TEST(rtc_calendar, ALateSecondOfTheRange) {
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(1791300000UL, &tm));
    expectTm(&tm, 2026, 10, 6, 15, 20, 0, 2);
}

TEST(rtc_calendar, OutsideTheRangeIsRefused) {
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcUnixToTm(946684799UL, &tm));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcUnixToTm(4102444800UL, &tm));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcUnixToTm(0, &tm));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcUnixToTm(4294967295UL, &tm));
}

TEST(rtc_calendar, TheLeapDayOf2000Exists) {
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(951782400UL, &tm));
    expectTm(&tm, 2000, 2, 29, 0, 0, 0, 2);
    TEST_ASSERT_EQUAL_UINT32(951782400UL, unixOf(2000, 2, 29, 0, 0, 0));
}

TEST(rtc_calendar, TheLeapDayOf2096Exists) {
    TEST_ASSERT_EQUAL_UINT32(3981357296UL, unixOf(2096, 2, 29, 12, 34, 56));
    TEST_ASSERT_EQUAL_UINT32(3981398400UL, unixOf(2096, 3, 1, 0, 0, 0));
    rtc_tm_t tm;
    TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(3981357296UL, &tm));
    expectTm(&tm, 2096, 2, 29, 12, 34, 56, 3);
}

TEST(rtc_calendar, FebruaryHasNoLeapDayIn2100) {
    rtc_tm_t tm = {2100, 2, 29, 0, 0, 0, 0};
    uint32_t t;
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&tm, &t));
}

TEST(rtc_calendar, FebruaryHasNoLeapDayInACommonYear) {
    uint32_t t;
    rtc_tm_t a = {2099, 2, 29, 0, 0, 0, 0};
    rtc_tm_t b = {2001, 2, 29, 0, 0, 0, 0};
    rtc_tm_t c = {2097, 2, 29, 0, 0, 0, 0};
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&a, &t));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&b, &t));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&c, &t));
}

TEST(rtc_calendar, EveryFieldIsRangeChecked) {
    uint32_t t;
    rtc_tm_t bad[] = {
        {1999, 12, 31, 23, 59, 59, 0}, {2100, 1, 1, 0, 0, 0, 0},
        {2026, 0, 1, 0, 0, 0, 0},      {2026, 13, 1, 0, 0, 0, 0},
        {2026, 1, 0, 0, 0, 0, 0},      {2026, 1, 32, 0, 0, 0, 0},
        {2026, 4, 31, 0, 0, 0, 0},     {2026, 1, 1, 24, 0, 0, 0},
        {2026, 1, 1, 0, 60, 0, 0},     {2026, 1, 1, 0, 0, 60, 0},
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&bad[i], &t));
}

TEST(rtc_calendar, EveryMonthEndsOnItsLastDay) {
    static const uint8_t common[] = {31, 28, 31, 30, 31, 30,
                                     31, 31, 30, 31, 30, 31};
    for (unsigned year = 2096; year <= 2097; year++) {
        for (unsigned mon = 1; mon <= 12; mon++) {
            unsigned last = common[mon - 1];
            if (mon == 2 && year == 2096)
                last = 29;
            uint32_t t;
            rtc_tm_t ok = {
                (uint16_t)year, (uint8_t)mon, (uint8_t)last, 0, 0, 0, 0};
            rtc_tm_t past = ok;
            past.mday = (uint8_t)(last + 1);
            TEST_ASSERT_EQUAL_INT(0, RtcTmToUnix(&ok, &t));
            TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&past, &t));
        }
    }
}

TEST(rtc_calendar, TheLastValidFieldsAreAccepted) {
    TEST_ASSERT_EQUAL_UINT32(4102444799UL, unixOf(2099, 12, 31, 23, 59, 59));
    TEST_ASSERT_EQUAL_UINT32(946684800UL, unixOf(2000, 1, 1, 0, 0, 0));
    unixOf(2026, 4, 30, 23, 59, 59);
    unixOf(2026, 1, 31, 0, 0, 0);
}

TEST(rtc_calendar, NullArgumentsAreRefused) {
    rtc_tm_t tm = {2026, 1, 1, 0, 0, 0, 0};
    uint32_t t;
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcUnixToTm(946684800UL, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(NULL, &t));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcTmToUnix(&tm, NULL));
}

/* Every day boundary, and the second before and after it, against libc. */
TEST(rtc_calendar, AgreesWithGmtimeAcrossTheRange) {
    for (uint32_t day = 0; day <= (RTC_UNIX_MAX - RTC_UNIX_MIN) / 86400U;
         day++) {
        uint32_t base = (uint32_t)RTC_UNIX_MIN + day * 86400U;
        uint32_t probes[] = {base, base + 1, base + 43200U, base + 86399U};
        for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
            uint32_t t = probes[i];
            if (t > RTC_UNIX_MAX)
                continue;
            time_t ref = (time_t)t;
            const struct tm *g = gmtime(&ref);
            rtc_tm_t tm;
            TEST_ASSERT_EQUAL_INT(0, RtcUnixToTm(t, &tm));
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_year + 1900U, tm.year);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_mon + 1U, tm.mon);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_mday, tm.mday);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_hour, tm.hour);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_min, tm.min);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_sec, tm.sec);
            TEST_ASSERT_EQUAL_UINT((unsigned)g->tm_wday, tm.wday);
            uint32_t back = 0;
            TEST_ASSERT_EQUAL_INT(0, RtcTmToUnix(&tm, &back));
            TEST_ASSERT_EQUAL_UINT32(t, back);
        }
    }
}

TEST_GROUP_RUNNER(rtc_calendar) {
    RUN_TEST_CASE(rtc_calendar, RangeStartIsTheYear2000);
    RUN_TEST_CASE(rtc_calendar, RangeEndIsTheLastSecondOf2099);
    RUN_TEST_CASE(rtc_calendar, ALateSecondOfTheRange);
    RUN_TEST_CASE(rtc_calendar, OutsideTheRangeIsRefused);
    RUN_TEST_CASE(rtc_calendar, TheLeapDayOf2000Exists);
    RUN_TEST_CASE(rtc_calendar, TheLeapDayOf2096Exists);
    RUN_TEST_CASE(rtc_calendar, FebruaryHasNoLeapDayIn2100);
    RUN_TEST_CASE(rtc_calendar, FebruaryHasNoLeapDayInACommonYear);
    RUN_TEST_CASE(rtc_calendar, EveryFieldIsRangeChecked);
    RUN_TEST_CASE(rtc_calendar, EveryMonthEndsOnItsLastDay);
    RUN_TEST_CASE(rtc_calendar, TheLastValidFieldsAreAccepted);
    RUN_TEST_CASE(rtc_calendar, NullArgumentsAreRefused);
    RUN_TEST_CASE(rtc_calendar, AgreesWithGmtimeAcrossTheRange);
}

/***************************************/
TEST_GROUP(rtc_grammar);
/***************************************/

TEST_SETUP(rtc_grammar) {}
TEST_TEAR_DOWN(rtc_grammar) {}

static int parse(const char *line, uint32_t *sec, rtc_source_t *src) {
    return RtcParseWrite(line, strlen(line), sec, src);
}

static void expectParsed(const char *line, uint32_t want,
                         rtc_source_t wantSrc) {
    uint32_t sec = 0;
    rtc_source_t src = RTC_SRC_NONE;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, parse(line, &sec, &src), line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(want, sec, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(wantSrc, src, line);
}

static void expectRefused(const char *line) {
    uint32_t sec = 7;
    rtc_source_t src = RTC_SRC_SNTP;
    TEST_ASSERT_EQUAL_INT_MESSAGE(-EINVAL, parse(line, &sec, &src), line);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7, sec, line);
    TEST_ASSERT_EQUAL_INT_MESSAGE(RTC_SRC_SNTP, src, line);
}

TEST(rtc_grammar, SecondsAloneDefaultToManual) {
    expectParsed("1791300000", 1791300000UL, RTC_SRC_MANUAL);
    expectParsed("1791300000\n", 1791300000UL, RTC_SRC_MANUAL);
}

TEST(rtc_grammar, EachSourceIsAccepted) {
    expectParsed("1791300000 rtc", 1791300000UL, RTC_SRC_RTC);
    expectParsed("1791300000 sntp", 1791300000UL, RTC_SRC_SNTP);
    expectParsed("1791300000 server", 1791300000UL, RTC_SRC_SERVER);
    expectParsed("1791300000 manual", 1791300000UL, RTC_SRC_MANUAL);
    expectParsed("1791300000 sntp\n", 1791300000UL, RTC_SRC_SNTP);
}

TEST(rtc_grammar, TheRangeEdgesAreAccepted) {
    expectParsed("946684800", 946684800UL, RTC_SRC_MANUAL);
    expectParsed("4102444799", 4102444799UL, RTC_SRC_MANUAL);
}

TEST(rtc_grammar, ValuesOutsideTheRangeAreRefused) {
    expectRefused("946684799");
    expectRefused("4102444800");
    expectRefused("0");
    expectRefused("99999999999");
    expectRefused("18446744073709551616");
    expectRefused("4294967296");
}

TEST(rtc_grammar, SignFractionAndNonDigitsAreRefused) {
    expectRefused("+1791300000");
    expectRefused("-1791300000");
    expectRefused("1791300000.5");
    expectRefused("1791300000.");
    expectRefused("17913x0000");
    expectRefused("0x6aa9c0a0");
    expectRefused("1e9");
}

TEST(rtc_grammar, EmptyAndMisplacedSpacesAreRefused) {
    expectRefused("");
    expectRefused("\n");
    expectRefused(" ");
    expectRefused(" 1791300000");
    expectRefused("1791300000 ");
    expectRefused("1791300000  sntp");
    expectRefused("1791300000\tsntp");
    expectRefused("1791300000 \n");
}

TEST(rtc_grammar, UnknownCutAndExtraSourcesAreRefused) {
    expectRefused("1791300000 none");
    expectRefused("1791300000 SNTP");
    expectRefused("1791300000 sn");
    expectRefused("1791300000 sntpx");
    expectRefused("1791300000 ntp");
    expectRefused("1791300000 sntp extra");
    expectRefused("1791300000 sntp\n\n");
    expectRefused("1791300000\n\n");
    expectRefused("1791300000\nsntp");
}

TEST(rtc_grammar, AnEmbeddedNulIsRefused) {
    uint32_t sec = 0;
    rtc_source_t src = RTC_SRC_NONE;
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          RtcParseWrite("1791300000\0 sntp", 16, &sec, &src));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcParseWrite("17913\0"
                                                 "00000",
                                                 11, &sec, &src));
}

TEST(rtc_grammar, LengthIsTheBufferNotAStringEnd) {
    uint32_t sec = 0;
    rtc_source_t src = RTC_SRC_NONE;
    TEST_ASSERT_EQUAL_INT(0,
                          RtcParseWrite("1791300000 sntpjunk", 15, &sec, &src));
    TEST_ASSERT_EQUAL_INT(RTC_SRC_SNTP, src);
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcParseWrite("1791300000", 0, &sec, &src));
}

TEST(rtc_grammar, ADigitPastTheLengthIsNotRead) {
    uint32_t sec = 0;
    rtc_source_t src = RTC_SRC_NONE;
    TEST_ASSERT_EQUAL_INT(0, RtcParseWrite("17913000001", 10, &sec, &src));
    TEST_ASSERT_EQUAL_UINT32(1791300000UL, sec);
    TEST_ASSERT_EQUAL_INT(RTC_SRC_MANUAL, src);
}

TEST(rtc_grammar, NullArgumentsAreRefused) {
    uint32_t sec;
    rtc_source_t src;
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcParseWrite(NULL, 3, &sec, &src));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcParseWrite("1791300000", 10, NULL, &src));
    TEST_ASSERT_EQUAL_INT(-EINVAL, RtcParseWrite("1791300000", 10, &sec, NULL));
}

TEST_GROUP_RUNNER(rtc_grammar) {
    RUN_TEST_CASE(rtc_grammar, SecondsAloneDefaultToManual);
    RUN_TEST_CASE(rtc_grammar, EachSourceIsAccepted);
    RUN_TEST_CASE(rtc_grammar, TheRangeEdgesAreAccepted);
    RUN_TEST_CASE(rtc_grammar, ValuesOutsideTheRangeAreRefused);
    RUN_TEST_CASE(rtc_grammar, SignFractionAndNonDigitsAreRefused);
    RUN_TEST_CASE(rtc_grammar, EmptyAndMisplacedSpacesAreRefused);
    RUN_TEST_CASE(rtc_grammar, UnknownCutAndExtraSourcesAreRefused);
    RUN_TEST_CASE(rtc_grammar, AnEmbeddedNulIsRefused);
    RUN_TEST_CASE(rtc_grammar, LengthIsTheBufferNotAStringEnd);
    RUN_TEST_CASE(rtc_grammar, ADigitPastTheLengthIsNotRead);
    RUN_TEST_CASE(rtc_grammar, NullArgumentsAreRefused);
}

/***************************************/
TEST_GROUP(rtc_source);
/***************************************/

TEST_SETUP(rtc_source) {}
TEST_TEAR_DOWN(rtc_source) {}

TEST(rtc_source, QualityFollowsTheSource) {
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC,
                            RtcSourceQuality(RTC_SRC_RTC));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SNTP_CALIBRATED,
                            RtcSourceQuality(RTC_SRC_SNTP));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION,
                            RtcSourceQuality(RTC_SRC_SERVER));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION,
                            RtcSourceQuality(RTC_SRC_MANUAL));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED,
                            RtcSourceQuality(RTC_SRC_NONE));
}

TEST(rtc_source, NamesAreTheTextTheNodeReads) {
    TEST_ASSERT_EQUAL_STRING("none", RtcSourceName(RTC_SRC_NONE));
    TEST_ASSERT_EQUAL_STRING("rtc", RtcSourceName(RTC_SRC_RTC));
    TEST_ASSERT_EQUAL_STRING("sntp", RtcSourceName(RTC_SRC_SNTP));
    TEST_ASSERT_EQUAL_STRING("server", RtcSourceName(RTC_SRC_SERVER));
    TEST_ASSERT_EQUAL_STRING("manual", RtcSourceName(RTC_SRC_MANUAL));
}

TEST(rtc_source, ASourceOutOfRangeIsNone) {
    rtc_source_t past = (rtc_source_t)(RTC_SRC_MANUAL + 1);
    TEST_ASSERT_EQUAL_STRING("none", RtcSourceName(past));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, RtcSourceQuality(past));
    TEST_ASSERT_EQUAL_STRING("none", RtcSourceName((rtc_source_t)99));
}

TEST_GROUP_RUNNER(rtc_source) {
    RUN_TEST_CASE(rtc_source, QualityFollowsTheSource);
    RUN_TEST_CASE(rtc_source, NamesAreTheTextTheNodeReads);
    RUN_TEST_CASE(rtc_source, ASourceOutOfRangeIsNone);
}
