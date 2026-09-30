/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>
#include <wasi/wasi-internal.h>
#include <wasi/wasi_types.h>

/* random_get over a stand-in for the platform fill: short fills are retried,
 * and a fill that makes no progress is an error rather than a hang. */

#define CALL_LIMIT 100

static int calls;
static size_t chunk;
static int64_t reply;

/* Writes `chunk` bytes of 0xA5 (or the remainder) and returns that count. */
static int64_t stubFill(uint8_t *buf, size_t len) {
    if (++calls > CALL_LIMIT)
        TEST_FAIL_MESSAGE("the fill was called without end");
    size_t n = chunk < len ? chunk : len;
    memset(buf, 0xA5, n);
    return (int64_t)n;
}

/* Returns a fixed reply and writes nothing. */
static int64_t fixedFill(uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    if (++calls > CALL_LIMIT)
        TEST_FAIL_MESSAGE("the fill was called without end");
    return reply;
}

TEST_GROUP(wasi_random);

TEST_SETUP(wasi_random) {
    calls = 0;
    chunk = 0;
    reply = 0;
}

TEST_TEAR_DOWN(wasi_random) {}

TEST(wasi_random, AFullFillTakesOneCall) {
    uint8_t buf[32] = {0};
    chunk = sizeof(buf);

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiRandomFill(stubFill, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(1, calls);
    for (size_t i = 0; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL_HEX8(0xA5, buf[i]);
}

TEST(wasi_random, ShortFillsAreRetriedUntilTheBufferIsFull) {
    uint8_t buf[10] = {0};
    chunk = 3;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiRandomFill(stubFill, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(4, calls);
    for (size_t i = 0; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL_HEX8(0xA5, buf[i]);
}

TEST(wasi_random, AFillThatMakesNoProgressIsAnIoError) {
    uint8_t buf[8];
    reply = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_IO,
                             WasiRandomFill(fixedFill, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(1, calls);
}

TEST(wasi_random, ANegativeErrnoBecomesTheGuestsErrno) {
    uint8_t buf[8];

    reply = -EIO;
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_IO,
                             WasiRandomFill(fixedFill, buf, sizeof(buf)));
    reply = -EINVAL;
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_INVAL,
                             WasiRandomFill(fixedFill, buf, sizeof(buf)));
}

TEST(wasi_random, ZeroLengthSucceedsWithoutAskingThePlatform) {
    uint8_t buf[1];

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiRandomFill(fixedFill, buf, 0));
    TEST_ASSERT_EQUAL_INT(0, calls);
}

TEST(wasi_random, ThePlatformFillFillsABuffer) {
    uint8_t buf[32];
    memset(buf, 0, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT16(
        __WASI_ERRNO_SUCCESS,
        WasiRandomFill(PlatfromGetRandom, buf, sizeof(buf)));
    uint8_t zero[32] = {0};
    TEST_ASSERT_TRUE(memcmp(buf, zero, sizeof(buf)) != 0);
}

TEST_GROUP_RUNNER(wasi_random) {
    RUN_TEST_CASE(wasi_random, AFullFillTakesOneCall);
    RUN_TEST_CASE(wasi_random, ShortFillsAreRetriedUntilTheBufferIsFull);
    RUN_TEST_CASE(wasi_random, AFillThatMakesNoProgressIsAnIoError);
    RUN_TEST_CASE(wasi_random, ANegativeErrnoBecomesTheGuestsErrno);
    RUN_TEST_CASE(wasi_random, ZeroLengthSucceedsWithoutAskingThePlatform);
    RUN_TEST_CASE(wasi_random, ThePlatformFillFillsABuffer);
}
