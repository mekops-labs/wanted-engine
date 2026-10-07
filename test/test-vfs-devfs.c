/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-autoconf.h>

/* The /dev table holds the engine's built-in entries and a full launch-config
 * drivers[] section at once. */

TEST_GROUP(vfs_devfs_capacity);

static vfs_ctx_t vfs;

TEST_SETUP(vfs_devfs_capacity) { vfs = VfsInit(); }
TEST_TEAR_DOWN(vfs_devfs_capacity) { VfsDestroy(&vfs); }

static int registerNamed(const char *name) {
    vfs_driver_t *d = VfsNullInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(d);
    int rc = DevFs_Register(vfs, name, d);
    if (rc != 0)
        d->Destroy(d);
    return rc;
}

TEST(vfs_devfs_capacity, TheBuiltinsAndAFullDriversSectionFit) {
    const char *builtins[] = {"null", "pipe", "stdin", "stdout", "stderr"};
    TEST_ASSERT_EQUAL_UINT(VFS_DEVFS_BUILTINS,
                           sizeof(builtins) / sizeof(builtins[0]));
    for (size_t i = 0; i < VFS_DEVFS_BUILTINS; i++)
        TEST_ASSERT_EQUAL_INT(0, registerNamed(builtins[i]));
    for (int i = 0; i < CONFIG_WANTED_MAX_DRIVERS_CNT; i++) {
        char name[16];
        snprintf(name, sizeof(name), "d%d", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, registerNamed(name), name);
    }
}

TEST(vfs_devfs_capacity, OneMoreThanThatIsRefused) {
    for (int i = 0; i < VFS_DEVFS_MAX_ENTRIES; i++) {
        char name[16];
        snprintf(name, sizeof(name), "e%d", i);
        TEST_ASSERT_EQUAL_INT(0, registerNamed(name));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, registerNamed("extra"));
}

TEST_GROUP_RUNNER(vfs_devfs_capacity) {
    RUN_TEST_CASE(vfs_devfs_capacity, TheBuiltinsAndAFullDriversSectionFit);
    RUN_TEST_CASE(vfs_devfs_capacity, OneMoreThanThatIsRefused);
}
