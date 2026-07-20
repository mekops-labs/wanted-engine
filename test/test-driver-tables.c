/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <vfs.h>
#include <vfs-drivers.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * driver_tables — the resolver's core → platform → extra search order.
 *
 * The extra table lets a build link drivers from a source tree outside this
 * repo. It is searched last, so a core name it claims must lose. Configure a
 * build with -DWANTED_EXTRA_DRIVERS_DIR=test/extra-drivers to exercise the
 * populated case; without it ExtraDriverTable() is NULL and the extra-specific
 * assertions do not apply.
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(driver_tables);

static vfs_ctx_t ctx;
static wapp_t *wapp;

TEST_SETUP(driver_tables) {
    ctx = VfsInit();
    TEST_ASSERT_NOT_NULL(ctx);
    wapp = WantedMalloc(sizeof(wapp_t));
    TEST_ASSERT_NOT_NULL(wapp);
    memset(wapp, 0, sizeof(*wapp));
}

TEST_TEAR_DOWN(driver_tables) {
    WantedFree(wapp);
    VfsDestroy(&ctx);
}

/* The core `null` device reads back nothing. The fixture extra table claims
 * that same name and would answer with a marker body, so a non-empty read here
 * means the search order let an out-of-tree table shadow a core driver. */
TEST(driver_tables, CoreNameWinsOverExtra) {
    char buf[16] = {0};
    int fd;

    TEST_ASSERT_EQUAL_INT(
        0, WantedInstallDriver(ctx, wapp, "null", "/dev/null", NULL));

    fd = VfsOpen(ctx, "/dev/null", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_EQUAL_INT(0, VfsRead(ctx, fd, buf, sizeof(buf)));
    VfsClose(ctx, fd);
}

/* A name only the extra table offers resolves when a tree is linked in, and is
 * cleanly unavailable when none is. */
TEST(driver_tables, ExtraNameResolvesOnlyWhenLinked) {
    int ret = WantedInstallDriver(ctx, wapp, "extra", "/dev/extra", NULL);

    if (ExtraDriverTable() == NULL) {
        TEST_ASSERT_EQUAL_INT(-ENODEV, ret);
        return;
    }

    char buf[16] = {0};
    TEST_ASSERT_EQUAL_INT(0, ret);

    int fd = VfsOpen(ctx, "/dev/extra", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, fd);
    TEST_ASSERT_EQUAL_INT(5, VfsRead(ctx, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("EXTRA", buf);
    VfsClose(ctx, fd);
}

/* Extra drivers are advertised on /proc/wanted alongside core and platform
 * ones, so a supervisor sees the build's real capability set. */
TEST(driver_tables, ListDriversReportsExtraDrivers) {
    char buf[256] = {0};

    TEST_ASSERT_GREATER_THAN_INT(0, WantedListDrivers(buf, sizeof(buf)));
    TEST_ASSERT_NOT_NULL(strstr(buf, "null"));

    if (ExtraDriverTable() == NULL) {
        TEST_ASSERT_NULL(strstr(buf, "extra"));
        return;
    }
    TEST_ASSERT_NOT_NULL(strstr(buf, "extra"));
}

TEST_GROUP_RUNNER(driver_tables) {
    RUN_TEST_CASE(driver_tables, CoreNameWinsOverExtra);
    RUN_TEST_CASE(driver_tables, ExtraNameResolvesOnlyWhenLinked);
    RUN_TEST_CASE(driver_tables, ListDriversReportsExtraDrivers);
}
