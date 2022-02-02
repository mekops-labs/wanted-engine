#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>

#include "vfs/vfs-internal.h"

/***************************************/
TEST_GROUP(vfs_internal);
/***************************************/

#ifdef WANTED_ROMFS

file_t fs[] = {
    {".",    0, VFS_FILETYPE_DIRECTORY,         -1},
    {"dev",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"xyz",  1, VFS_FILETYPE_CHARACTER_DEVICE,   1},
    {"net",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"sock", 3, VFS_FILETYPE_SOCKET_STREAM,      2},
    {"sys",  0, VFS_FILETYPE_DIRECTORY,         -1},
    {"bus",  5, VFS_FILETYPE_SOCKET_DGRAM,       0},
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
    int i = VfsFindFile(0, "not_a_file", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFile(0, "/not_a_file", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFile(0, "/dev/a", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFile(0, "/dev/xyzz", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFile(1, "dev/xyz", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_internal, findFileRoot)
{
    int i = VfsFindFile(0, "/", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFile(0, ".", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindFile(0, "..", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindFile(0, "./.", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(0, i);
}

TEST(vfs_internal, findFileDir)
{
    int i = VfsFindFile(0, "/dev", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindFile(0, "dev", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindFile(0, "/../dev", fs, fsLen);
    TEST_ASSERT_EQUAL_INT(1, i);
}

#endif

TEST_GROUP_RUNNER(vfs_internal)
{
#ifdef WANTED_ROMFS
    RUN_TEST_CASE(vfs_internal, findFileNotFound);
    RUN_TEST_CASE(vfs_internal, findFileRoot);
    RUN_TEST_CASE(vfs_internal, findFileDir);
#endif
}
