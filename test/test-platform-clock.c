/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * platform_clock — deterministic monotonic counter
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(platform_clock);

TEST_SETUP(platform_clock) { DummyClockReset(); }
TEST_TEAR_DOWN(platform_clock) {}

TEST(platform_clock, Res_ReturnsOneMsForRealtimeAndMonotonic) {
    uint64_t res = 0;
    TEST_ASSERT_EQUAL_INT(0, PlatformClockGetRes(PLAT_CLOCKID_REALTIME, &res));
    TEST_ASSERT_EQUAL_UINT64(1000000ULL, res);

    res = 0;
    TEST_ASSERT_EQUAL_INT(0, PlatformClockGetRes(PLAT_CLOCKID_MONOTONIC, &res));
    TEST_ASSERT_EQUAL_UINT64(1000000ULL, res);
}

TEST(platform_clock, Res_InvalidId_ReturnsEinval) {
    uint64_t res = 0;
    TEST_ASSERT_EQUAL_INT(-EINVAL, PlatformClockGetRes(99, &res));
}

TEST(platform_clock, GetTime_AdvancesBy1msPerCall) {
    plat_timestamp_t t0, t1, t2;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t0);
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t1);
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t2);
    TEST_ASSERT_EQUAL_UINT64(t0 + 1000000ULL, t1);
    TEST_ASSERT_EQUAL_UINT64(t1 + 1000000ULL, t2);
}

TEST(platform_clock, GetTime_StartsAtZeroAfterReset) {
    plat_timestamp_t t;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t);
    TEST_ASSERT_EQUAL_UINT64(0, t);
}

TEST(platform_clock, GetTime_RealtimeAndMonotonicShareCounter) {
    plat_timestamp_t mono, real;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &mono);
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &real);
    /* REALTIME is MONOTONIC + one step (counter auto-advances per call) */
    TEST_ASSERT_EQUAL_UINT64(mono + 1000000ULL, real);
}

TEST(platform_clock, Sleep_AdvancesCounter) {
    plat_timestamp_t before, after;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &before);
    PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, 5000000ULL, 0); /* 5 ms */
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &after);
    /* first GetTime auto-advances clock by 1ms, sleep adds 5ms;
     * second GetTime reads clock (before+6ms) then advances after return */
    TEST_ASSERT_EQUAL_UINT64(before + 6000000ULL, after);
}

TEST(platform_clock, ClockAdvance_IncreasesCounter) {
    plat_timestamp_t t0, t1;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t0);
    DummyClockAdvance(10000000ULL); /* 10 ms */
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &t1);
    TEST_ASSERT_EQUAL_UINT64(t0 + 11000000ULL, t1);
}

#define NS_PER_S 1000000000ULL

TEST(platform_clock, SetTime_RealtimeReadsBackTheSetValue) {
    TEST_ASSERT_EQUAL_INT(0, PlatformClockSetTime(PLAT_CLOCKID_REALTIME,
                                                  1791300000ULL * NS_PER_S));
    plat_timestamp_t t;
    TEST_ASSERT_EQUAL_INT(0, PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &t));
    TEST_ASSERT_EQUAL_UINT64(1791300000ULL * NS_PER_S, t);
}

TEST(platform_clock, SetTime_RealtimeKeepsRunningAfterTheSet) {
    PlatformClockSetTime(PLAT_CLOCKID_REALTIME, 1791300000ULL * NS_PER_S);
    plat_timestamp_t a, b;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &a);
    DummyClockAdvance(5 * NS_PER_S);
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &b);
    TEST_ASSERT_EQUAL_UINT64(a + 5 * NS_PER_S + 1000000ULL, b);
}

TEST(platform_clock, SetTime_DoesNotMoveMonotonic) {
    DummyClockAdvance(3 * NS_PER_S);
    PlatformClockSetTime(PLAT_CLOCKID_REALTIME, 1791300000ULL * NS_PER_S);
    plat_timestamp_t mono;
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &mono);
    TEST_ASSERT_EQUAL_UINT64(3 * NS_PER_S, mono);
}

TEST(platform_clock, SetTime_OnlyRealtimeIsSettable) {
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, PlatformClockSetTime(PLAT_CLOCKID_MONOTONIC, NS_PER_S));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PlatformClockSetTime(99, NS_PER_S));
}

TEST(platform_clock, SetTime_InjectedFailureLeavesTheClock) {
    plat_timestamp_t before;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &before);
    DummyClockFailSet(-EPERM);
    TEST_ASSERT_EQUAL_INT(
        -EPERM,
        PlatformClockSetTime(PLAT_CLOCKID_REALTIME, 1791300000ULL * NS_PER_S));
    plat_timestamp_t after;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &after);
    TEST_ASSERT_EQUAL_UINT64(before + 1000000ULL, after);
}

TEST(platform_clock, Reset_ClearsTheSetTimeAndTheInjectedFailure) {
    PlatformClockSetTime(PLAT_CLOCKID_REALTIME, 1791300000ULL * NS_PER_S);
    DummyClockFailSet(-EPERM);
    DummyClockReset();
    TEST_ASSERT_EQUAL_INT(
        0, PlatformClockSetTime(PLAT_CLOCKID_REALTIME, NS_PER_S));
    DummyClockReset();
    plat_timestamp_t t;
    PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &t);
    TEST_ASSERT_EQUAL_UINT64(0, t);
}

TEST_GROUP_RUNNER(platform_clock) {
    RUN_TEST_CASE(platform_clock, Res_ReturnsOneMsForRealtimeAndMonotonic);
    RUN_TEST_CASE(platform_clock, Res_InvalidId_ReturnsEinval);
    RUN_TEST_CASE(platform_clock, GetTime_AdvancesBy1msPerCall);
    RUN_TEST_CASE(platform_clock, GetTime_StartsAtZeroAfterReset);
    RUN_TEST_CASE(platform_clock, GetTime_RealtimeAndMonotonicShareCounter);
    RUN_TEST_CASE(platform_clock, Sleep_AdvancesCounter);
    RUN_TEST_CASE(platform_clock, ClockAdvance_IncreasesCounter);
    RUN_TEST_CASE(platform_clock, SetTime_RealtimeReadsBackTheSetValue);
    RUN_TEST_CASE(platform_clock, SetTime_RealtimeKeepsRunningAfterTheSet);
    RUN_TEST_CASE(platform_clock, SetTime_DoesNotMoveMonotonic);
    RUN_TEST_CASE(platform_clock, SetTime_OnlyRealtimeIsSettable);
    RUN_TEST_CASE(platform_clock, SetTime_InjectedFailureLeavesTheClock);
    RUN_TEST_CASE(platform_clock, Reset_ClearsTheSetTimeAndTheInjectedFailure);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * platform_rng — deterministic xorshift32
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(platform_rng);

TEST_SETUP(platform_rng) { DummyClockReset(); }
TEST_TEAR_DOWN(platform_rng) {}

TEST(platform_rng, FillsBuffer_NonZeroOutput) {
    uint8_t buf[8] = {0};
    int64_t n = PlatfromGetRandom(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT64(8, n);

    int all_zero = 1;
    for (int i = 0; i < 8; i++) {
        if (buf[i] != 0) {
            all_zero = 0;
            break;
        }
    }
    TEST_ASSERT_FALSE(all_zero);
}

TEST(platform_rng, TwoCallsProduce_DifferentBytes) {
    uint8_t a[4] = {0}, b[4] = {0};
    PlatfromGetRandom(a, sizeof(a));
    PlatfromGetRandom(b, sizeof(b));
    TEST_ASSERT_FALSE(memcmp(a, b, 4) == 0);
}

TEST(platform_rng, DeterministicAfterReset) {
    uint8_t first[4] = {0};
    uint8_t second[4] = {0};
    PlatfromGetRandom(first, sizeof(first));
    DummyClockReset();
    PlatfromGetRandom(second, sizeof(second));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(first, second, 4);
}

TEST_GROUP_RUNNER(platform_rng) {
    RUN_TEST_CASE(platform_rng, FillsBuffer_NonZeroOutput);
    RUN_TEST_CASE(platform_rng, TwoCallsProduce_DifferentBytes);
    RUN_TEST_CASE(platform_rng, DeterministicAfterReset);
}
