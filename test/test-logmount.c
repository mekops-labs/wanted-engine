/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>

/* Exercises the read-only log-mount driver (vfs-logmount.c) directly over the
 * process-wide LogStore. */

static vfs_driver_t *drv;

static void seed(const char *name, const char *text) {
    LogStoreAppend(LogStore(), name, text, strlen(text));
}

TEST_GROUP(logmount);

TEST_SETUP(logmount) { drv = NULL; }

TEST_TEAR_DOWN(logmount) {
    if (drv && drv->Destroy)
        drv->Destroy(drv);
}

TEST(logmount, ReadsAWappLog) {
    seed("lm_a", "hello-a");
    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);

    int fd = drv->Open(drv->ctx, "/lm_a", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[64] = {0};
    int n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "hello-a"));
    /* Second read is EOF. */
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, fd, buf, sizeof(buf)));
    drv->Close(drv->ctx, fd);
}

/* Regression: a reader looping with a buffer smaller than the ring's content
 * (`wsh cat`'s 1024-byte loop, e.g.) used to see the oldest chunk once, then
 * a permanent EOF latch — everything past the first read was invisible. */
TEST(logmount, LoopingReadsPageThroughTheWholeLog) {
    char long_msg[200];
    memset(long_msg, 'x', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';
    seed("lm_long", long_msg);

    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
    int fd = drv->Open(drv->ctx, "/lm_long", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    /* A buffer far smaller than the log, read in a loop until EOF (0). */
    char small[16];
    char collected[512] = {0};
    size_t total = 0;
    int n;
    while ((n = drv->Read(drv->ctx, fd, small, sizeof(small))) > 0) {
        TEST_ASSERT_TRUE(total + (size_t)n < sizeof(collected));
        memcpy(collected + total, small, (size_t)n);
        total += (size_t)n;
    }
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(collected, long_msg));
    drv->Close(drv->ctx, fd);
}

TEST(logmount, RootEnumeratesSlots) {
    seed("lm_a", "a");
    seed("lm_b", "b");
    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);

    int fd = drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[256];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "lm_a", 4));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "lm_b", 4));
    drv->Close(drv->ctx, fd);
}

TEST(logmount, ScopeNarrowsToOneWapp) {
    seed("lm_a", "a");
    seed("lm_b", "b");
    drv = VfsLogMountInit(NULL, "name=lm_a");
    TEST_ASSERT_NOT_NULL(drv);

    /* The scoped wapp is reachable; a sibling is not. */
    int fd = drv->Open(drv->ctx, "/lm_a", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    drv->Close(drv->ctx, fd);
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, "/lm_b", VFS_O_RDONLY));

    /* readdir lists only the scoped wapp. */
    fd = drv->Open(drv->ctx, "/", VFS_O_RDONLY);
    uint8_t buf[256];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "lm_a", 4));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "lm_b", 4));
    drv->Close(drv->ctx, fd);
}

TEST(logmount, UnknownWappReturnsEnoent) {
    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "/nope_xyz", VFS_O_RDONLY));
}

TEST(logmount, WriteOpenRejected) {
    seed("lm_a", "a");
    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Open(drv->ctx, "/lm_a", VFS_O_WRONLY));
    /* The driver exposes no write entry point. */
    TEST_ASSERT_NULL(drv->Write);
}

/* The engine's own error channel is readable through the same mount as a
 * wapp's, under a name no installable image can claim — a board with no
 * console has no other way to be asked what happened. */
TEST(logmount, ReadsTheEngineLog) {
    WantedLogCapture("wanted: an engine fault\n", 24);
    drv = VfsLogMountInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);

    int fd = drv->Open(drv->ctx, "/" WANTED_ENGINE_LOG_NAME, VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    /* The ring is process-wide and other groups share it, so a read must
     * cover the whole cap to be sure of seeing this call's own write. */
    char buf[CONFIG_WANTED_LOG_CAP + 1] = {0};
    int n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "an engine fault"));
    drv->Close(drv->ctx, fd);
}

/* A grant may scope to the engine's log alone, giving neither wapp output nor
 * the control plane. */
TEST(logmount, ScopeNarrowsToTheEngineLog) {
    WantedLogCapture("wanted: scoped\n", 15);
    seed("lm_other", "not-this");
    drv = VfsLogMountInit(NULL, "name=" WANTED_ENGINE_LOG_NAME);
    TEST_ASSERT_NOT_NULL(drv);

    int fd = drv->Open(drv->ctx, "/" WANTED_ENGINE_LOG_NAME, VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    drv->Close(drv->ctx, fd);

    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "/lm_other", VFS_O_RDONLY));
}

TEST_GROUP_RUNNER(logmount) {
    RUN_TEST_CASE(logmount, ReadsTheEngineLog);
    RUN_TEST_CASE(logmount, ScopeNarrowsToTheEngineLog);
    RUN_TEST_CASE(logmount, ReadsAWappLog);
    RUN_TEST_CASE(logmount, LoopingReadsPageThroughTheWholeLog);
    RUN_TEST_CASE(logmount, RootEnumeratesSlots);
    RUN_TEST_CASE(logmount, ScopeNarrowsToOneWapp);
    RUN_TEST_CASE(logmount, UnknownWappReturnsEnoent);
    RUN_TEST_CASE(logmount, WriteOpenRejected);
}
