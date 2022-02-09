#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <romfs.h>

#include "external_symbols.h"

/***************************************/
TEST_GROUP(vfs_internal);
/***************************************/

file_t fs[] = {
    {"/",    0,     NULL,    },
    {"dev",  1,     NULL,    },
    {"xyz",  2,     NULL,    },
    {"dir",  1,     NULL,    },
    {"net",  1,     NULL,    },
    {"sock", 2,     NULL,    },
    {"rom",  1,     NULL,    },
    {"sys",  1,     NULL,    },
    {"bus",  2,     NULL,    },
    {".dotfile", 1, NULL,    },
    {"drv",  1,     &vfs_dummy_drv,  },
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
    int i = VfsFindEntryAt(0, "not_a_file", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/not_a_file", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/a", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dir/dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/net/bus", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/xyzz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(1, "dev/xyzz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "../dev/xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/dev/../xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "..", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_internal, findFileRoot)
{
    int i = VfsFindEntryAt(0, "/", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, "./.", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".dotfile", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(9, i);

    i = VfsFindEntryAt(0, "/dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntryAt(0, "dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntryAt(0, "/./dev", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntryAt(0, "////./dir", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);
}

TEST(vfs_internal, findFileDir)
{
    int i = VfsFindEntryAt(1, "xyz", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntryAt(1, "../net", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(4, i);

    i = VfsFindEntryAt(1, "../", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(1, "..", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(1, "../././.", fs, fsLen, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);
}

TEST(vfs_internal, findFileDriver)
{
    const char *drvPath = NULL;

    int i = VfsFindEntryAt(0, "/drv/a", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING(".", drvPath);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv/x/y/z", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("x/y/z", drvPath);

    i = VfsFindEntryAt(0, "/dir", fs, fsLen, &drvPath);
    TEST_ASSERT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(3, i);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/drv/file/x", fs, fsLen, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(10, i);
    TEST_ASSERT_EQUAL_STRING("file/x", drvPath);
}

TEST_GROUP_RUNNER(vfs_internal)
{
    RUN_TEST_CASE(vfs_internal, findFileNotFound);
    RUN_TEST_CASE(vfs_internal, findFileRoot);
    RUN_TEST_CASE(vfs_internal, findFileDir);
    RUN_TEST_CASE(vfs_internal, findFileDriver);
}


/***************************************/
TEST_GROUP(vfs_openclose);
/***************************************/

TEST_SETUP(vfs_openclose)
{
    RomfsLoad(test_wasi_romfs, test_wasi_romfs_len);
}

TEST_TEAR_DOWN(vfs_openclose)
{
}

TEST(vfs_openclose, OpenFail)
{
    int i = VfsOpen("xxx", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE(fildes[3].opened);
    TEST_ASSERT_EQUAL_PTR(NULL, fildes[3].drv);

    i = VfsOpen("/roms", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE(fildes[3].opened);
    TEST_ASSERT_EQUAL_PTR(NULL, fildes[3].drv);
}

TEST(vfs_openclose, OpenThenClose)
{
    int i = VfsOpen("/", 0);
    TEST_ASSERT_EQUAL_INT(3, i);
    TEST_ASSERT_TRUE(fildes[3].opened);
    TEST_ASSERT_NULL(fildes[3].drv);

    i = VfsOpen("/rom", 0);
    TEST_ASSERT_EQUAL_INT(4, i);
    TEST_ASSERT_TRUE(fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(&vfs_romfs_drv, fildes[4].drv);

    i = VfsClose(3);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(fildes[3].opened);
    TEST_ASSERT_NULL(fildes[3].drv);

    i = VfsClose(4);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(fildes[4].opened);
    TEST_ASSERT_NULL(fildes[4].drv);
}

TEST_GROUP_RUNNER(vfs_openclose)
{
    RUN_TEST_CASE(vfs_openclose, OpenFail);
    RUN_TEST_CASE(vfs_openclose, OpenThenClose);
}
