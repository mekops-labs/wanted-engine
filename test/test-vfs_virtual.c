#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <vfs-drivers.h>
#include <vfs/vfs-virtual.h>

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
    const char id[] = {'V', 'i', 'r', 't'};

    ret = VfsVirtualInit(&v);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_MEMORY(id, v.id, sizeof(id));

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

    TEST_ASSERT_NULL(fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("", fs[0].name);

    ret = TRY_DRV(&virt, Register, "/", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    ret = TRY_DRV(&virt, Register, "..", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    ret = TRY_DRV(&virt, Register, ".", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    for (int i = 0; i < MAX_ENTRIES + 1; i++) {
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
    TEST_ASSERT_EQUAL(0, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy, fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("a", fs[0].name);
}

TEST(vfs_virtual_register, RegisterMultiple)
{
    int ret;
    vfs_driver_t dummy1;
    vfs_driver_t dummy2;
    vfs_entry_t *fs = virt.ctx->entries;

    ret = TRY_DRV(&virt, Register, "abc", &dummy1);
    TEST_ASSERT_EQUAL(0, ret);

    ret = TRY_DRV(&virt, Register, "ab", &dummy2);
    TEST_ASSERT_EQUAL(1, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy1, fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("abc", fs[0].name);

    TEST_ASSERT_EQUAL_PTR(&dummy2, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("ab", fs[1].name);

    TEST_ASSERT_NULL(fs[2].drv);
}

TEST(vfs_virtual_register, RegisterTree)
{
    int ret;
    vfs_driver_t dummy = { .Register = NULL };

    VfsVirtualInit(&virt2);

    ret = TRY_DRV(&virt, Register, "a", &virt2);
    TEST_ASSERT_EQUAL(0, ret);

    ret = TRY_DRV(&virt, Register, "a/b", &dummy);
    TEST_ASSERT_EQUAL(0, ret);

    vfs_entry_t *fs = virt2.ctx->entries;
    TEST_ASSERT_EQUAL_PTR(&dummy, fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("b", fs[0].name);

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
    TEST_ASSERT_EQUAL_INT(6, i);

    i = VfsFindEntryAt(0, "/c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntryAt(0, "c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);
}

TEST(vfs_virtual_find, findFileDir)
{
    vfs_entry_t *fs = virt.ctx->entries;
    vfs_entry_t *fs2 = virt2.ctx->entries;
    const char *pathLeft;

    int i = VfsFindEntryAt(1, "c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntryAt(0, "dir/xyz", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(4, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntryAt(0, "dir/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(4, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

     i = VfsFindEntryAt(0, "dev/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntryAt(0, "dev/aa", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);

    i = VfsFindEntryAt(0, pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual_find, findFileDriver)
{
    vfs_entry_t *fs = virt.ctx->entries;

    const char *drvPath = NULL;

    int i = VfsFindEntryAt(0, "/dev/a", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);
    TEST_ASSERT_EQUAL_PTR(&virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/dev/.", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING(".", drvPath);
    TEST_ASSERT_EQUAL_PTR(&virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntryAt(0, "/dir/x/y/z", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(4, i);
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
