#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <vfs-drivers.h>
#include <vfs/vfs-virtual.h>

#include <path_utils.h>

#include "external_symbols.h"

static vfs_driver_t virt, virt2;
static vfs_driver_t dummy_test;

/***************************************/
TEST_GROUP(vfs_virtual_init);
/***************************************/

TEST_SETUP(vfs_virtual_init)
{

}

TEST_TEAR_DOWN(vfs_virtual_init)
{
}

TEST(vfs_virtual_init, InitAndDestroy)
{
    int ret;
    vfs_driver_t v;

    ret = VfsVirtualInit(&v);
    vfs_entry_t *fs = v.ctx->entries;

    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_MEMORY("Virt", v.id, 4);

    TEST_ASSERT_EQUAL_PTR(&v, fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("/", fs[0].name);

    VfsVirtualDestroy(&v);
    TEST_ASSERT_NULL(v.ctx);
}

TEST_GROUP_RUNNER(vfs_virtual_init)
{
    RUN_TEST_CASE(vfs_virtual_init, InitAndDestroy);
}

/***************************************/
TEST_GROUP(vfs_virtual_register);
/***************************************/

TEST_SETUP(vfs_virtual_register)
{
    VfsVirtualInit(&virt);
}

TEST_TEAR_DOWN(vfs_virtual_register)
{
    VfsVirtualDestroy(&virt);
}

TEST(vfs_virtual_register, RegisterFail)
{
    int ret;
    vfs_driver_t dummy;
    vfs_entry_t *fs = virt.ctx->entries;

    ret = TRY_DRV(&virt, Register, "", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    TEST_ASSERT_NULL(fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("", fs[1].name);

    ret = TRY_DRV(&virt, Register, "/", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    ret = TRY_DRV(&virt, Register, "..", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    ret = TRY_DRV(&virt, Register, ".", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    for (int i = 0; i < MAX_ENTRIES; i++) {
        char d[2] = {'0' + i, '\0'};
        ret = TRY_DRV(&virt, Register, d, &dummy);
    }
    TEST_ASSERT_EQUAL(-ENFILE, ret);
}

TEST(vfs_virtual_register, RegisterSingle)
{
    int ret;
    vfs_driver_t dummy;
    vfs_entry_t *fs = virt.ctx->entries;

    ret = TRY_DRV(&virt, Register, "a", &dummy);
    TEST_ASSERT_EQUAL(1, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("a", fs[1].name);
}

TEST(vfs_virtual_register, RegisterMultiple)
{
    int ret;
    vfs_driver_t dummy1;
    vfs_driver_t dummy2;
    vfs_entry_t *fs = virt.ctx->entries;

    ret = TRY_DRV(&virt, Register, "abc", &dummy1);
    TEST_ASSERT_EQUAL(1, ret);

    ret = TRY_DRV(&virt, Register, "ab", &dummy2);
    TEST_ASSERT_EQUAL(2, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy1, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("abc", fs[1].name);

    TEST_ASSERT_EQUAL_PTR(&dummy2, fs[2].drv);
    TEST_ASSERT_EQUAL_STRING("ab", fs[2].name);

    TEST_ASSERT_NULL(fs[3].drv);
}

TEST(vfs_virtual_register, RegisterTree)
{
    int ret;
    vfs_driver_t dummy = { .Register = NULL };

    VfsVirtualInit(&virt2);

    ret = TRY_DRV(&virt, Register, "a", &virt2);
    TEST_ASSERT_EQUAL(1, ret);

    ret = TRY_DRV(&virt, Register, "a/b", &dummy);
    TEST_ASSERT_EQUAL(1, ret);

    vfs_entry_t *fs = virt2.ctx->entries;
    TEST_ASSERT_EQUAL_PTR(&dummy, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("b", fs[1].name);

    ret = TRY_DRV(&virt, Register, "a/b/c", &dummy);
    TEST_ASSERT_EQUAL(-EPERM, ret);

    VfsVirtualDestroy(&virt2);
}

TEST_GROUP_RUNNER(vfs_virtual_register)
{
    RUN_TEST_CASE(vfs_virtual_register, RegisterFail);
    RUN_TEST_CASE(vfs_virtual_register, RegisterSingle);
    RUN_TEST_CASE(vfs_virtual_register, RegisterMultiple);
    RUN_TEST_CASE(vfs_virtual_register, RegisterTree);
}

/***************************************/
TEST_GROUP(vfs_virtual_find);
/***************************************/

TEST_SETUP(vfs_virtual_find)
{
    VfsVirtualInit(&virt);
    VfsVirtualInit(&virt2);
    TRY_DRV(&virt, Register, "a", &dummy_test);
    TRY_DRV(&virt, Register, "b", &dummy_test);
    TRY_DRV(&virt, Register, "c", &dummy_test);
    TRY_DRV(&virt, Register, "net", &dummy_test);
    TRY_DRV(&virt, Register, "dir", &virt2);
    TRY_DRV(&virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(&virt, Register, "dev", &virt2);
    TRY_DRV(&virt, Register, "dev/a", &dummy_test);
    TRY_DRV(&virt, Register, ".dotfile", &dummy_test);

}

TEST_TEAR_DOWN(vfs_virtual_find)
{
    VfsVirtualDestroy(&virt2);
    VfsVirtualDestroy(&virt);
}

TEST(vfs_virtual_find, findFileNotFound)
{
    int i;
    vfs_entry_t *fs = virt.ctx->entries;
    const char *pathLeft;

    i = VfsFindEntryAt(0, "..", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "/n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "aa", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "dir/xyz", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntryAt(0, "../dev/xyz", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual_find, findFileRoot)
{
    vfs_entry_t *fs = virt.ctx->entries;

    int i = VfsFindEntryAt(0, "/", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, ".dotfile", fs, NULL);
    TEST_ASSERT_EQUAL_INT(7, i);

    i = VfsFindEntryAt(0, "/c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntryAt(0, "c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);
}

TEST(vfs_virtual_find, findFileDir)
{
    vfs_entry_t *fs = virt.ctx->entries;
    vfs_entry_t *fs2 = virt2.ctx->entries;
    const char *pathLeft;

    int i = VfsFindEntryAt(1, "c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntryAt(0, "dir/xyz", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntryAt(0, "dir/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

     i = VfsFindEntryAt(0, "dev/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(6, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntryAt(0, "dev/aa", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(6, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual_find, findFileDriver)
{
    vfs_entry_t *fs = virt.ctx->entries;

    const char *drvPath = NULL;

    int i = VfsFindEntryAt(0, "/dev/a", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);
    TEST_ASSERT_EQUAL_PTR(&virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/dev/.", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING(".", drvPath);
    TEST_ASSERT_EQUAL_PTR(&virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/dir/x/y/z", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("x/y/z", drvPath);
    TEST_ASSERT_EQUAL_PTR(&virt2, fs[i].drv);

    drvPath = NULL;
}

TEST_GROUP_RUNNER(vfs_virtual_find)
{
    RUN_TEST_CASE(vfs_virtual_find, findFileNotFound);
    RUN_TEST_CASE(vfs_virtual_find, findFileRoot);
    RUN_TEST_CASE(vfs_virtual_find, findFileDir);
    RUN_TEST_CASE(vfs_virtual_find, findFileDriver);
}


/***************************************/
TEST_GROUP(vfs_virtual_open);
/***************************************/

TEST_SETUP(vfs_virtual_open)
{
    VfsVirtualInit(&virt);
    VfsVirtualInit(&virt2);
    TRY_DRV(&virt, Register, "a", &dummy_test);
    TRY_DRV(&virt, Register, "b", &dummy_test);
    TRY_DRV(&virt, Register, "c", &dummy_test);
    TRY_DRV(&virt, Register, "net", &dummy_test);
    TRY_DRV(&virt, Register, "dir", &virt2);
    TRY_DRV(&virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(&virt, Register, "dev", &virt2);
    TRY_DRV(&virt, Register, "dev/a", &dummy_test);
    TRY_DRV(&virt, Register, ".dotfile", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_open)
{
    VfsVirtualDestroy(&virt2);
    VfsVirtualDestroy(&virt);
}

TEST(vfs_virtual_open, OpenFail)
{
    int r;
    char path_too_long[MAX_PATH_LEN+1];

    r = TRY_DRV(&virt, Open, "", 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(&virt, Open, NULL, 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(&virt, Open, "..", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(&virt, Open, "x", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(&virt, OpenAt, MAX_ENTRIES+1, "a", 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(&virt, OpenAt, MAX_ENTRIES+1, "a", 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    memset(path_too_long, 'a', sizeof(path_too_long));
    path_too_long[MAX_PATH_LEN] = '\0';

    r = TRY_DRV(&virt, OpenAt, 0, path_too_long, 0);
    TEST_ASSERT_EQUAL(-ENAMETOOLONG, r);

    r = TRY_DRV(&virt, OpenAt, 0, "a/b", 0);
    TEST_ASSERT_EQUAL(-EPERM, r);

    r = TRY_DRV(&virt, OpenAt, 0, "dir/b", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(&virt, OpenAt, 0, "dir/../x", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(&virt, OpenAt, 6, "dir", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);
}

TEST(vfs_virtual_open, OpenSimple)
{
    int r;

    r = TRY_DRV(&virt, Open, "/", 0);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(&virt, Open, ".", 0);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(&virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(&virt, Open, "dir", 0);
    TEST_ASSERT_EQUAL(5, r);

    r = TRY_DRV(&virt, Open, ".dotfile", 0);
    TEST_ASSERT_EQUAL(7, r);
}

TEST(vfs_virtual_open, OpenAdvanced)
{
    int r;

    r = TRY_DRV(&virt, Open, "dir/xyz", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(&virt, Open, "dir/../dev", 0);
    TEST_ASSERT_EQUAL(6, r);

    r = TRY_DRV(&virt, Open, "/./////dir///../dev/////", 0);
    TEST_ASSERT_EQUAL(6, r);

     r = TRY_DRV(&virt, Open, "dir/../dev/xyz", 0);
    TEST_ASSERT_EQUAL(1, r);
}

TEST_GROUP_RUNNER(vfs_virtual_open)
{
    RUN_TEST_CASE(vfs_virtual_open, OpenFail);
    RUN_TEST_CASE(vfs_virtual_open, OpenSimple);
    RUN_TEST_CASE(vfs_virtual_open, OpenAdvanced);
}


/***************************************/
TEST_GROUP(vfs_virtual_statat);
/***************************************/

TEST_SETUP(vfs_virtual_statat)
{
    VfsVirtualInit(&virt);
    VfsVirtualInit(&virt2);
    dummy_test.filetype = 0;
    dummy_test.bytesId = 0;
    TRY_DRV(&virt, Register, "a", &dummy_test);
    TRY_DRV(&virt, Register, "b", &dummy_test);
    TRY_DRV(&virt, Register, "c", &dummy_test);
    TRY_DRV(&virt, Register, "net", &dummy_test);
    TRY_DRV(&virt, Register, "dir", &virt2);
    TRY_DRV(&virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(&virt, Register, "dev", &virt2);
    TRY_DRV(&virt, Register, "dev/a", &dummy_test);
    TRY_DRV(&virt, Register, ".dotfile", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_statat)
{
    VfsVirtualDestroy(&virt2);
    VfsVirtualDestroy(&virt);
}

TEST(vfs_virtual_statat, StatAtFail)
{
    int r;
    vfs_filestat_t s;
    char path_too_long[MAX_PATH_LEN+1];

    r = TRY_DRV(&virt, FileStatAt, 0, NULL, &s);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(&virt, FileStatAt, 0, "", &s);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(&virt, FileStatAt, 0, "a", NULL);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(&virt, FileStatAt, 0, "x", &s);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(&virt, FileStatAt, 0, "/dir/aa", &s);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    memset(path_too_long, 'a', sizeof(path_too_long));
    path_too_long[MAX_PATH_LEN] = '\0';

    r = TRY_DRV(&virt, FileStatAt, 0, path_too_long, &s);
    TEST_ASSERT_EQUAL(-ENAMETOOLONG, r);

    r = TRY_DRV(&virt, FileStatAt, 0, "a/b", &s);
    TEST_ASSERT_EQUAL(-EPERM, r);
}

TEST(vfs_virtual_statat, StatAtSimple)
{
    int r;
    vfs_filestat_t s;

    dummy_test.filetype = VFS_FILETYPE_REGULAR_FILE;
    dummy_test.bytesId = 1234;

    r = TRY_DRV(&virt, FileStatAt, 0, ".", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, s.filetype);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &s.dev, 4);
    TEST_ASSERT_EQUAL(0, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "/", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, s.filetype);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &s.dev, 4);
    TEST_ASSERT_EQUAL(0, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "//////.", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, s.filetype);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &s.dev, 4);
    TEST_ASSERT_EQUAL(0, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "a", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_REGULAR_FILE, s.filetype);
    TEST_ASSERT_EQUAL(1234, s.dev);
    TEST_ASSERT_EQUAL(1, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "dir", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, s.filetype);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &s.dev, 4);
    TEST_ASSERT_EQUAL(5, s.ino);
}

TEST(vfs_virtual_statat, StatAtAdvanced)
{
    int r;
    vfs_filestat_t s;

    dummy_test.filetype = VFS_FILETYPE_REGULAR_FILE;
    dummy_test.bytesId = 1234;

    r = TRY_DRV(&virt, FileStatAt, 0, "dir/xyz", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_REGULAR_FILE, s.filetype);
    TEST_ASSERT_EQUAL(1234, s.dev);
    TEST_ASSERT_EQUAL(1, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "dir/..", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, s.filetype);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &s.dev, 4);
    TEST_ASSERT_EQUAL(0, s.ino);

    r = TRY_DRV(&virt, FileStatAt, 0, "/dir/../dev/../dir/a", &s);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_REGULAR_FILE, s.filetype);
    TEST_ASSERT_EQUAL(1234, s.dev);
    TEST_ASSERT_EQUAL(2, s.ino);
}

TEST_GROUP_RUNNER(vfs_virtual_statat)
{
    RUN_TEST_CASE(vfs_virtual_statat, StatAtFail);
    RUN_TEST_CASE(vfs_virtual_statat, StatAtSimple);
    RUN_TEST_CASE(vfs_virtual_statat, StatAtAdvanced);
}
