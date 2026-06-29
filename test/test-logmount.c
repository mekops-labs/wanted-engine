/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>

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
    TEST_ASSERT_EQUAL_INT(-EROFS,
                          drv->Open(drv->ctx, "/lm_a", VFS_O_WRONLY));
    /* The driver exposes no write entry point. */
    TEST_ASSERT_NULL(drv->Write);
}

TEST_GROUP_RUNNER(logmount) {
    RUN_TEST_CASE(logmount, ReadsAWappLog);
    RUN_TEST_CASE(logmount, RootEnumeratesSlots);
    RUN_TEST_CASE(logmount, ScopeNarrowsToOneWapp);
    RUN_TEST_CASE(logmount, UnknownWappReturnsEnoent);
    RUN_TEST_CASE(logmount, WriteOpenRejected);
}
