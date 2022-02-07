#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>

#include "vfs/vfs-internal.h"

/***************************************/
TEST_GROUP(vfs_internal);
/***************************************/

vfs_driver_t dummy = {

};

#ifdef WANTED_ROMFS

file_t fs[] = {
    {"/",    0, VFS_FILETYPE_DIRECTORY,         NULL},
    {"dev",  1, VFS_FILETYPE_DIRECTORY,         NULL},
    {"xyz",  2, VFS_FILETYPE_CHARACTER_DEVICE,  NULL},
    {"dir",  1, VFS_FILETYPE_DIRECTORY,         NULL},
    {"net",  1, VFS_FILETYPE_DIRECTORY,         NULL},
    {"sock", 2, VFS_FILETYPE_SOCKET_STREAM,     NULL},
    {"rom",  1, VFS_FILETYPE_DIRECTORY,         NULL},
    {"sys",  1, VFS_FILETYPE_DIRECTORY,         NULL},
    {"bus",  2, VFS_FILETYPE_SOCKET_DGRAM,      NULL},
    {".dotfile", 1, VFS_FILETYPE_REGULAR_FILE,  NULL},
    {"drv",  1, VFS_FILETYPE_DIRECTORY,         &dummy},
};

const size_t fsLen = sizeof(fs)/sizeof(fs[0]);

TEST_SETUP(vfs_internal)
{
}

TEST_TEAR_DOWN(vfs_internal)
{
}

TEST(vfs_internal, findFileNotFound)
{
    int i = VfsFindFileAt(0, "not_a_file", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/not_a_file", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/dev/a", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/dir/dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/net/bus", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/dev/xyzz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(1, "dev/xyzz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "../dev/xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "/dev/../xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFileAt(0, "..", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_internal, findFileRoot)
{
    int i = VfsFindFileAt(0, "/", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFileAt(0, ".", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFileAt(0, "./.", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFileAt(0, ".dotfile", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(9, i);

    i = VfsFindFileAt(0, "/dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindFileAt(0, "dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindFileAt(0, "/./dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindFileAt(0, "/dir/../dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindFileAt(0, "////./dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindFileAt(0, ".//////dir/..////./dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);
}

TEST(vfs_internal, findFileDir)
{
    int i = VfsFindFileAt(1, "xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindFileAt(1, "../net", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(4, i);

    i = VfsFindFileAt(1, "../", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFileAt(1, "..", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFileAt(1, "../././.", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);
}

TEST(vfs_internal, findFileDriver)
{
    const char *drvPath = NULL;

    int i = VfsFindFileAt(0, "/drv/a", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);

    drvPath = NULL;

    i = VfsFindFileAt(0, "/drv", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("", drvPath);

    drvPath = NULL;

    i = VfsFindFileAt(0, "/drv/x/y/z", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("x/y/z", drvPath);

    i = VfsFindFileAt(0, "/dir", fs, fsLen, &drvPath);
    TEST_ASSERT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(3, i);

    drvPath = NULL;

    i = VfsFindFileAt(0, "/drv/../dir", fs, fsLen, &drvPath);
    TEST_ASSERT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(3, i);
}



/***************************************/
TEST_GROUP(vfs_ops);
/***************************************/

extern vfs_entry_t fildes[];

TEST_SETUP(vfs_ops)
{
}

TEST_TEAR_DOWN(vfs_ops)
{
}

TEST(vfs_ops, Open)
{
    int i = VfsOpen("/", 0);
    TEST_ASSERT_EQUAL_INT(3, i);
}

TEST_GROUP_RUNNER(vfs_ops)
{
    RUN_TEST_CASE(vfs_ops, Open);
}


#endif

TEST_GROUP_RUNNER(vfs_internal)
{
#ifdef WANTED_ROMFS
    RUN_TEST_CASE(vfs_internal, findFileNotFound);
    RUN_TEST_CASE(vfs_internal, findFileRoot);
    RUN_TEST_CASE(vfs_internal, findFileDir);
    RUN_TEST_CASE(vfs_internal, findFileDriver);
#endif
}
