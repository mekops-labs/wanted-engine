/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <string.h>

#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>

static wapp_t make_wapp(const char *name) {
    wapp_t w;
    memset(&w, 0, sizeof(w));
    strncpy(w.name, name, WAPP_MAX_NAME_LEN - 1);
    return w;
}

/* Step over the "[+<ms>] " a line carries, so a test asserts on the payload.
 * The value moves with the clock and is never asserted on directly. */
static const char *afterStamp(const char *s) {
    const char *sp = strchr(s, ' ');
    return (s[0] == '[' && s[1] == '+' && sp != NULL) ? sp + 1 : s;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_log_driver — log console driver (src/vfs/vfs-log.c)
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(vfs_log_driver);

static vfs_driver_t *drv;

TEST_SETUP(vfs_log_driver) {
    wapp_t w = make_wapp("testapp");
    drv = VfsLogInit(&w, NULL);
    TEST_ASSERT_NOT_NULL(drv);
}

TEST_TEAR_DOWN(vfs_log_driver) {
    if (drv) {
        drv->Destroy(drv);
        drv = NULL;
    }
}

TEST(vfs_log_driver, InitNullWappReturnsNull) {
    vfs_driver_t *d = VfsLogInit(NULL, NULL);
    TEST_ASSERT_NULL(d);
}

TEST(vfs_log_driver, InitSetsCharacterDeviceFiletype) {
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, drv->filetype);
}

TEST(vfs_log_driver, OpenSucceeds) {
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "", 0));
}

TEST(vfs_log_driver, ReadReturnsEof) {
    char buf[64];
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_log_driver, StatReturnsCharacterDevice) {
    vfs_stat_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, 0, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, st.filetype);
}

TEST(vfs_log_driver, WriteReturnsByteCount) {
    const char msg[] = "hello";
    int r = drv->Write(drv->ctx, 0, msg, sizeof(msg) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), r);
}

TEST(vfs_log_driver, WriteAppendsToLogStore) {
    /* Use a unique slot name so accumulated writes from other tests don't
     * inflate the byte count — LogStore is a process-wide singleton. */
    wapp_t w = make_wapp("log-wt");
    vfs_driver_t *d = VfsLogInit(&w, NULL);
    TEST_ASSERT_NOT_NULL(d);

    const char msg[] = "logged output";
    d->Write(d->ctx, 0, msg, sizeof(msg) - 1);

    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), "log-wt", out, sizeof(out) - 1);
    TEST_ASSERT_GREATER_THAN_size_t(sizeof(msg) - 1, n);
    TEST_ASSERT_EQUAL_STRING(msg, afterStamp(out));

    d->Destroy(d);
}

TEST(vfs_log_driver, WriteNullBufIsIgnored) {
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, 0, NULL, 4) < 0);
}

TEST_GROUP_RUNNER(vfs_log_driver) {
    RUN_TEST_CASE(vfs_log_driver, InitNullWappReturnsNull);
    RUN_TEST_CASE(vfs_log_driver, InitSetsCharacterDeviceFiletype);
    RUN_TEST_CASE(vfs_log_driver, OpenSucceeds);
    RUN_TEST_CASE(vfs_log_driver, ReadReturnsEof);
    RUN_TEST_CASE(vfs_log_driver, StatReturnsCharacterDevice);
    RUN_TEST_CASE(vfs_log_driver, WriteReturnsByteCount);
    RUN_TEST_CASE(vfs_log_driver, WriteAppendsToLogStore);
    RUN_TEST_CASE(vfs_log_driver, WriteNullBufIsIgnored);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * log_store — ring buffer store (src/log-store.c)
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(log_store);

TEST_SETUP(log_store) {}
TEST_TEAR_DOWN(log_store) {}

TEST(log_store, AppendAndReadRoundtrip) {
    const char data[] = "store roundtrip";
    LogStoreAppend(LogStore(), "rtt", data, sizeof(data) - 1);

    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), "rtt", out, sizeof(out) - 1);
    TEST_ASSERT_GREATER_THAN_size_t(sizeof(data) - 1, n);
    TEST_ASSERT_EQUAL_STRING(data, afterStamp(out));
}

TEST(log_store, EachLineCarriesItsOwnStamp) {
    LogStoreAppend(LogStore(), "stamp2", "one\ntwo\n", 8);

    char out[64] = {0};
    LogStoreRead(LogStore(), "stamp2", out, sizeof(out) - 1);

    const char *first = afterStamp(out);
    TEST_ASSERT_EQUAL_STRING_LEN("one\n", first, 4);
    TEST_ASSERT_EQUAL_STRING("two\n", afterStamp(first + 4));
}

/* A producer flushes when it likes, so a line split across writes must not be
 * stamped twice. */
TEST(log_store, AContinuedLineIsNotStampedAgain) {
    LogStoreAppend(LogStore(), "stamp3", "abc", 3);
    LogStoreAppend(LogStore(), "stamp3", "def\n", 4);

    char out[64] = {0};
    LogStoreRead(LogStore(), "stamp3", out, sizeof(out) - 1);
    TEST_ASSERT_EQUAL_STRING("abcdef\n", afterStamp(out));
}

TEST(log_store, AStampOpensTheLineAfterANewline) {
    LogStoreAppend(LogStore(), "stamp4", "done\n", 5);
    LogStoreAppend(LogStore(), "stamp4", "next", 4);

    char out[64] = {0};
    LogStoreRead(LogStore(), "stamp4", out, sizeof(out) - 1);

    const char *first = afterStamp(out);
    TEST_ASSERT_EQUAL_STRING_LEN("done\n", first, 5);
    TEST_ASSERT_EQUAL_STRING("next", afterStamp(first + 5));
}

TEST(log_store, UptimeAdvances) {
    uint64_t a = LogStoreUptimeMs();
    uint64_t b = LogStoreUptimeMs();
    TEST_ASSERT_TRUE(b >= a);
}

TEST(log_store, ReadAbsentNameReturnsZero) {
    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), "no-such-wapp", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(0, n);
}

TEST(log_store, AppendIsNonDestructiveRead) {
    const char data[] = "persistent";
    LogStoreAppend(LogStore(), "ndr", data, sizeof(data) - 1);

    char first[64] = {0}, second[64] = {0};
    LogStoreRead(LogStore(), "ndr", first, sizeof(first));
    LogStoreRead(LogStore(), "ndr", second, sizeof(second));
    TEST_ASSERT_EQUAL_MEMORY(first, second, sizeof(data) - 1);
}

TEST(log_store, AppendNullInputsAreNoOps) {
    /* Must not crash — NULL guards in LogStoreAppend. */
    LogStoreAppend(NULL, "x", "data", 4);
    LogStoreAppend(LogStore(), NULL, "data", 4);
    LogStoreAppend(LogStore(), "x", NULL, 4);
}

TEST_GROUP_RUNNER(log_store) {
    RUN_TEST_CASE(log_store, AppendAndReadRoundtrip);
    RUN_TEST_CASE(log_store, EachLineCarriesItsOwnStamp);
    RUN_TEST_CASE(log_store, AContinuedLineIsNotStampedAgain);
    RUN_TEST_CASE(log_store, AStampOpensTheLineAfterANewline);
    RUN_TEST_CASE(log_store, UptimeAdvances);
    RUN_TEST_CASE(log_store, ReadAbsentNameReturnsZero);
    RUN_TEST_CASE(log_store, AppendIsNonDestructiveRead);
    RUN_TEST_CASE(log_store, AppendNullInputsAreNoOps);
}
