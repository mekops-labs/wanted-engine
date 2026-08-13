/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <string.h>

#include <log-store.h>
#include <platform.h>
#include <wanted-autoconf.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * log_persist — the engine's ring across a reset
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(log_persist);

TEST_SETUP(log_persist) {
    DummyPersistMemClear();
    DummyResetReasonSet(NULL);
}

TEST_TEAR_DOWN(log_persist) {
    DummyPersistMemClear();
    DummyResetReasonSet(NULL);
}

/* The store is a process-wide singleton, so a test that wants a fresh boot
 * clears the memory a reset would have kept and re-runs the adoption. */
static void reboot(void) {
    LogStorePersistDetach(LogStore());
    LogStorePersistInit(LogStore());
}

TEST(log_persist, FirstBoot_ServesNoPreviousLog) {
    reboot();
    TEST_ASSERT_FALSE(LogStoreHas(LogStore(), WANTED_PREV_LOG_NAME));

    char out[64] = {0};
    TEST_ASSERT_EQUAL_size_t(
        0, LogStoreRead(LogStore(), WANTED_PREV_LOG_NAME, out, sizeof(out)));
}

TEST(log_persist, PreviousBootsLogSurvivesTheReset) {
    reboot();
    WantedLogCapture("wanted: the boot before\n", 24);

    /* The reset keeps the memory; the store adopts what was live. */
    reboot();
    TEST_ASSERT_TRUE(LogStoreHas(LogStore(), WANTED_PREV_LOG_NAME));

    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), WANTED_PREV_LOG_NAME, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(24, n);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: the boot before\n", out, 24);
}

TEST(log_persist, ThisBootsLinesDoNotAppearInThePreviousLog) {
    reboot();
    WantedLogCapture("wanted: older\n", 14);
    reboot();
    WantedLogCapture("wanted: newer\n", 14);

    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), WANTED_PREV_LOG_NAME, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(14, n);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: older\n", out, 14);
}

TEST(log_persist, PowerLossLeavesNoPreviousLog) {
    reboot();
    WantedLogCapture("wanted: before the power went\n", 30);

    /* Losing power clears the region, so the magic no longer matches. */
    DummyPersistMemClear();
    reboot();

    TEST_ASSERT_FALSE(LogStoreHas(LogStore(), WANTED_PREV_LOG_NAME));
}

TEST(log_persist, TheStoreIsListedOnlyWhenItHasAPreviousLog) {
    reboot();
    char names[CONFIG_WANTED_LOG_SLOTS + 1][WAPP_MAX_NAME_LEN];
    size_t n = LogStoreList(LogStore(), names, CONFIG_WANTED_LOG_SLOTS + 1);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_NOT_EQUAL_INT(
            0, strcmp(names[i], WANTED_PREV_LOG_NAME)); /* absent */
    }

    WantedLogCapture("wanted: something\n", 18);
    reboot();

    n = LogStoreList(LogStore(), names, CONFIG_WANTED_LOG_SLOTS + 1);
    bool found = false;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(names[i], WANTED_PREV_LOG_NAME) == 0)
            found = true;
    }
    TEST_ASSERT_TRUE(found);
}

TEST(log_persist, OnlyTheMostRecentBytesSurviveAFullRing) {
    reboot();
    /* Two capfuls: the first cannot survive the second. */
    char big[CONFIG_WANTED_LOG_PERSIST_CAP];
    memset(big, 'a', sizeof(big));
    WantedLogCapture(big, sizeof(big));
    WantedLogCapture("wanted: the last line\n", 22);
    reboot();

    char out[CONFIG_WANTED_LOG_PERSIST_CAP];
    size_t n = LogStoreRead(LogStore(), WANTED_PREV_LOG_NAME, out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(CONFIG_WANTED_LOG_PERSIST_CAP, n);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: the last line\n", out + n - 22, 22);
}

TEST(log_persist, TheEngineRingStaysReadableInRam) {
    reboot();
    WantedLogCapture("wanted: current boot\n", 21);

    char out[CONFIG_WANTED_LOG_CAP];
    size_t n =
        LogStoreRead(LogStore(), WANTED_ENGINE_LOG_NAME, out, sizeof(out));
    TEST_ASSERT_TRUE(n >= 21);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: current boot\n", out + n - 21, 21);
}

/* The failure this exists for: a task watchdog resets the board, and the boot
 * that follows must carry both the previous log and the reason. */
TEST(log_persist, AWatchdogResetKeepsTheLogAndNamesItself) {
    reboot();
    WantedLogCapture("wanted: the tick before the wedge\n", 33);

    DummyResetReasonSet("task_wdt");
    reboot();

    char prev[64] = {0};
    size_t n =
        LogStoreRead(LogStore(), WANTED_PREV_LOG_NAME, prev, sizeof(prev));
    TEST_ASSERT_EQUAL_size_t(33, n);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: the tick before the wedge\n", prev,
                                 33);

    /* This boot opens with why the last one ended. */
    char cur[CONFIG_WANTED_LOG_CAP];
    size_t m =
        LogStoreRead(LogStore(), WANTED_ENGINE_LOG_NAME, cur, sizeof(cur));
    TEST_ASSERT_TRUE(m >= 25);
    TEST_ASSERT_EQUAL_STRING_LEN("wanted: boot after task_wdt\n", cur + m - 28,
                                 28);
}

TEST(log_persist, APlatformWithNoResetReasonOpensNoLine) {
    DummyResetReasonSet(NULL);
    reboot();

    char cur[CONFIG_WANTED_LOG_CAP];
    size_t m =
        LogStoreRead(LogStore(), WANTED_ENGINE_LOG_NAME, cur, sizeof(cur));
    if (m >= 19) {
        TEST_ASSERT_NOT_EQUAL_INT(
            0, strncmp(cur + m - 19, "wanted: boot after ", 19));
    }
}

TEST_GROUP_RUNNER(log_persist) {
    RUN_TEST_CASE(log_persist, FirstBoot_ServesNoPreviousLog);
    RUN_TEST_CASE(log_persist, PreviousBootsLogSurvivesTheReset);
    RUN_TEST_CASE(log_persist, ThisBootsLinesDoNotAppearInThePreviousLog);
    RUN_TEST_CASE(log_persist, PowerLossLeavesNoPreviousLog);
    RUN_TEST_CASE(log_persist, TheStoreIsListedOnlyWhenItHasAPreviousLog);
    RUN_TEST_CASE(log_persist, OnlyTheMostRecentBytesSurviveAFullRing);
    RUN_TEST_CASE(log_persist, TheEngineRingStaysReadableInRam);
    RUN_TEST_CASE(log_persist, AWatchdogResetKeepsTheLogAndNamesItself);
    RUN_TEST_CASE(log_persist, APlatformWithNoResetReasonOpensNoLine);
}
