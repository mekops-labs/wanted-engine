#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <vfs-drivers.h>

#include "external_symbols.h"


static vfs_ctx_t vfs;
static vfs_driver_t romfs;
static vfs_driver_t virt1, virt2;

/***************************************/
TEST_GROUP(vfs_init);
/***************************************/

TEST_SETUP(vfs_init)
{
}

TEST_TEAR_DOWN(vfs_init)
{
}

TEST(vfs_init, InitAndDestroy)
{
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_EQUAL(c->fildes[0].drv_fd, 0);

    VfsDestroy(&c);
    TEST_ASSERT_NULL(c);
}

TEST_GROUP_RUNNER(vfs_init)
{
    RUN_TEST_CASE(vfs_init, InitAndDestroy);
}

/***************************************/
TEST_GROUP(vfs_register);
/***************************************/


TEST_SETUP(vfs_register)
{
    vfs = VfsInit();
}

TEST_TEAR_DOWN(vfs_register)
{
    VfsDestroy(&vfs);
}

TEST(vfs_register, SingleRoot)
{
    VfsVirtualInit(&virt1);
    VfsRegister(vfs, "/", &virt1);

    TEST_ASSERT_EQUAL_PTR(&virt1, vfs->fildes[3].drv);

    VfsVirtualDestroy(&virt1);
}

TEST(vfs_register, RootAndSingleVirtualDir)
{
    VfsVirtualInit(&virt1);
    VfsVirtualInit(&virt2);

    VfsRegister(vfs, "/", &virt1);
    VfsRegister(vfs, "/dir", &virt2);

    TEST_ASSERT_EQUAL_PTR(&virt1, vfs->fildes[3].drv);
    vfs_filestat_t stat;
    int f = TRY_DRV(&virt1, FileStatAt, 0, "dir", &stat);
    TEST_ASSERT_EQUAL(0, f);
    TEST_ASSERT_EQUAL(virt2.bytesId, stat.dev);

    VfsVirtualDestroy(&virt1);
    VfsVirtualDestroy(&virt2);
}

TEST_GROUP_RUNNER(vfs_register)
{
    RUN_TEST_CASE(vfs_register, SingleRoot);
    RUN_TEST_CASE(vfs_register, RootAndSingleVirtualDir);
}


/***************************************/
TEST_GROUP(vfs_openclose);
/***************************************/

TEST_SETUP(vfs_openclose)
{
    vfs = VfsInit();
    VfsVirtualInit(&virt1);
    VfsRomfsInit(&romfs, "/", test_wasi_romfs, test_wasi_romfs_len);
    VfsRegister(vfs, "/", &virt1);
    VfsRegister(vfs, "rom", &romfs);
}

TEST_TEAR_DOWN(vfs_openclose)
{
    VfsRomfsDestroy(&romfs);
    VfsVirtualDestroy(&virt1);
    VfsDestroy(&vfs);
}

TEST(vfs_openclose, OpenFail)
{
    int i = VfsOpen(vfs, "xxx", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE( vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(NULL,  vfs->fildes[4].drv);

    i = VfsOpen(vfs, "/roms", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE(vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(NULL,  vfs->fildes[4].drv);
}

TEST(vfs_openclose, OpenThenClose)
{
    int i;

    i = VfsOpen(vfs, "/rom", 0);
    TEST_ASSERT_EQUAL_INT(3, i);
    TEST_ASSERT_TRUE(vfs->fildes[3].opened);
    TEST_ASSERT_EQUAL_PTR(&virt1, vfs->fildes[3].drv);

    i = VfsOpen(vfs, "/", 0);
    TEST_ASSERT_EQUAL_INT(4, i);
    TEST_ASSERT_TRUE(vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(&virt1, vfs->fildes[4].drv);

    i = VfsOpen(vfs, "/rom/app.wasm", 0);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_TRUE(vfs->fildes[5].opened);
    TEST_ASSERT_EQUAL_PTR(&virt1, vfs->fildes[5].drv);

    i = VfsClose(vfs, 3);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(vfs->fildes[3].opened);
    TEST_ASSERT_TRUE(vfs->fildes[4].opened);

    i = VfsClose(vfs, 4);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(vfs->fildes[4].opened);

    i = VfsClose(vfs, 5);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(vfs->fildes[5].opened);
}

TEST_GROUP_RUNNER(vfs_openclose)
{
    RUN_TEST_CASE(vfs_openclose, OpenFail);
    RUN_TEST_CASE(vfs_openclose, OpenThenClose);
}
