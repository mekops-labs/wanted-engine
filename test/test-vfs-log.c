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

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_log_driver — log console driver (src/vfs/vfs-log.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

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
    size_t n = LogStoreRead(LogStore(), "log-wt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(sizeof(msg) - 1, n);
    TEST_ASSERT_EQUAL_STRING_LEN(msg, out, sizeof(msg) - 1);

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
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(log_store);

TEST_SETUP(log_store) {}
TEST_TEAR_DOWN(log_store) {}

TEST(log_store, AppendAndReadRoundtrip) {
    const char data[] = "store roundtrip";
    LogStoreAppend(LogStore(), "rtt", data, sizeof(data) - 1);

    char out[64] = {0};
    size_t n = LogStoreRead(LogStore(), "rtt", out, sizeof(out));
    TEST_ASSERT_EQUAL_size_t(sizeof(data) - 1, n);
    TEST_ASSERT_EQUAL_STRING_LEN(data, out, sizeof(data) - 1);
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
    RUN_TEST_CASE(log_store, ReadAbsentNameReturnsZero);
    RUN_TEST_CASE(log_store, AppendIsNonDestructiveRead);
    RUN_TEST_CASE(log_store, AppendNullInputsAreNoOps);
}
