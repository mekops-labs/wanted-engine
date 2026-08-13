/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include <platform.h>
#include <vfs.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * platform_wake — the descriptor a blocking wait watches beside its own
 * ═══════════════════════════════════════════════════════════════════════════
 */

TEST_GROUP(platform_wake);

TEST_SETUP(platform_wake) {}
TEST_TEAR_DOWN(platform_wake) {}

/* Whether `fd` is readable now. This is the question a blocking driver asks of
 * the wake descriptor, so the tests ask it the same way. */
static int readableNow(int fd) {
    fd_set r;
    struct timeval tv = {0, 0};

    FD_ZERO(&r);
    FD_SET(fd, &r);
    return select(fd + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(fd, &r);
}

TEST(platform_wake, Create_GivesADescriptorThatIsNotYetReadable) {
    int fd = PlatformWakeCreate();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_FALSE(readableNow(fd));
    PlatformWakeClose(fd);
}

TEST(platform_wake, Raise_MakesItReadable) {
    int fd = PlatformWakeCreate();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

    PlatformWakeRaise(fd);
    TEST_ASSERT_TRUE(readableNow(fd));
    PlatformWakeClose(fd);
}

TEST(platform_wake, Raise_StaysRaised) {
    int fd = PlatformWakeCreate();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);

    PlatformWakeRaise(fd);
    PlatformWakeRaise(fd);
    TEST_ASSERT_TRUE(readableNow(fd));
    TEST_ASSERT_TRUE(readableNow(fd));
    PlatformWakeClose(fd);
}

TEST(platform_wake, Raise_OnNoDescriptorIsIgnored) {
    PlatformWakeRaise(-1);
    PlatformWakeClose(-1);
}

TEST(platform_wake, Vfs_HasNoWakeDescriptorUntilOneIsSet) {
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(-1, VfsWakeFd(c));
    VfsDestroy(&c);
}

TEST(platform_wake, Vfs_CarriesTheWakeDescriptorAndClosesIt) {
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_NOT_NULL(c);

    int fd = PlatformWakeCreate();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    VfsSetWakeFd(c, fd);
    TEST_ASSERT_EQUAL_INT(fd, VfsWakeFd(c));

    PlatformWakeRaise(fd);
    TEST_ASSERT_TRUE(readableNow(fd));

    /* Destroy owns the descriptor; a second close would answer EBADF. */
    VfsDestroy(&c);
    TEST_ASSERT_FALSE(readableNow(fd));
}

TEST(platform_wake, Vfs_WakeFdOfNothingIsNoDescriptor) {
    TEST_ASSERT_EQUAL_INT(-1, VfsWakeFd(NULL));
}

TEST_GROUP_RUNNER(platform_wake) {
    RUN_TEST_CASE(platform_wake, Create_GivesADescriptorThatIsNotYetReadable);
    RUN_TEST_CASE(platform_wake, Raise_MakesItReadable);
    RUN_TEST_CASE(platform_wake, Raise_StaysRaised);
    RUN_TEST_CASE(platform_wake, Raise_OnNoDescriptorIsIgnored);
    RUN_TEST_CASE(platform_wake, Vfs_HasNoWakeDescriptorUntilOneIsSet);
    RUN_TEST_CASE(platform_wake, Vfs_CarriesTheWakeDescriptorAndClosesIt);
    RUN_TEST_CASE(platform_wake, Vfs_WakeFdOfNothingIsNoDescriptor);
}
