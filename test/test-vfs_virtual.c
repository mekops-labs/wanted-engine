#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <vfs-drivers.h>

#include "external_symbols.h"


vfs_driver_t test_drv;


/***************************************/
TEST_GROUP(vfs_virtual);
/***************************************/

TEST_SETUP(vfs_virtual)
{

}

TEST_TEAR_DOWN(vfs_virtual)
{
}

vfs_entry_t *fs;
TEST(vfs_virtual, findFileNotFound)
{
    int i = VfsFindEntryAt(0, "not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/a", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dir/dir", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/net/bus", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/xyzz", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(1, "dev/xyzz", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "../dev/xyz", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/../xyz", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "..", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual, findFileRoot)
{
    int i = VfsFindEntryAt(0, "/", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, "./.", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".dotfile", fs, NULL);
    TEST_ASSERT_EQUAL_INT(4, i);

    i = VfsFindEntryAt(0, "/d", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntryAt(0, "d", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntryAt(0, "/./d", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntryAt(0, "////./b", fs, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);
}

TEST(vfs_virtual, findFileDir)
{
    int i = VfsFindEntryAt(1, "c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    // i = VfsFindEntryAt(1, "../a", fs, NULL);
    // TEST_ASSERT_EQUAL_INT(4, i);

    // i = VfsFindEntryAt(1, "../", fs, NULL);
    // TEST_ASSERT_EQUAL_INT(0, i);

    // i = VfsFindEntryAt(1, "..", fs, NULL);
    // TEST_ASSERT_EQUAL_INT(0, i);

    // i = VfsFindEntryAt(1, "../././.", fs, NULL);
    // TEST_ASSERT_EQUAL_INT(0, i);
}

TEST(vfs_virtual, findFileDriver)
{
    const char *drvPath = NULL;

    int i = VfsFindEntryAt(0, "/drv/a", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING(".", drvPath);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv/x/y/z", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("x/y/z", drvPath);

    i = VfsFindEntryAt(0, "/d", fs, &drvPath);
    TEST_ASSERT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(3, i);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv/file/x", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("file/x", drvPath);
}

TEST_GROUP_RUNNER(vfs_virtual)
{
    // RUN_TEST_CASE(vfs_virtual, findFileNotFound);
    // RUN_TEST_CASE(vfs_virtual, findFileRoot);
    // RUN_TEST_CASE(vfs_virtual, findFileDir);
    // RUN_TEST_CASE(vfs_virtual, findFileDriver);
}
