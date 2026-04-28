#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-netfs.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

#include "external_symbols.h"

static vfs_ctx_t vfs;
static vfs_driver_t *romfs;
static vfs_driver_t *virt1, *virt2;

/***************************************/
TEST_GROUP(vfs_init);
/***************************************/

TEST_SETUP(vfs_init) {}

TEST_TEAR_DOWN(vfs_init) {}

TEST(vfs_init, InitAndDestroy) {
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_EQUAL(c->fildes[0].drv_fd, 0);

    VfsDestroy(&c);
    TEST_ASSERT_NULL(c);
}

TEST_GROUP_RUNNER(vfs_init) { RUN_TEST_CASE(vfs_init, InitAndDestroy); }

/***************************************/
TEST_GROUP(vfs_register);
/***************************************/

TEST_SETUP(vfs_register) { vfs = VfsInit(); }

TEST_TEAR_DOWN(vfs_register) { VfsDestroy(&vfs); }

TEST(vfs_register, SingleRoot) {
    virt1 = VfsVirtualInit(NULL, NULL);
    VfsRegister(vfs, "/", virt1);

    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[3].drv);
}

TEST(vfs_register, RootAndSingleVirtualDir) {
    virt1 = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);

    VfsRegister(vfs, "/", virt1);
    VfsRegister(vfs, "/dir", virt2);

    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[3].drv);

    vfs_stat_t stat;
    int f = TRY_DRV(virt1, Open, "dir", 0);
    f = TRY_DRV(virt1, Stat, f, &stat);
    TEST_ASSERT_EQUAL(0, f);
    TEST_ASSERT_EQUAL(virt2->bytesId, stat.dev);
}

TEST_GROUP_RUNNER(vfs_register) {
    RUN_TEST_CASE(vfs_register, SingleRoot);
    RUN_TEST_CASE(vfs_register, RootAndSingleVirtualDir);
}

/***************************************/
TEST_GROUP(vfs_openclose);
/***************************************/

TEST_SETUP(vfs_openclose) {
    /* Legacy romfs path coverage: this test exercises VfsRomfsInit, which
     * cannot parse the new TAR-format test_wasi blob. Phase 8 retires both
     * the romfs driver and this fixture. */
    wapp_t w = {.layers = {test_wasi_romfs},
                .layer_lens = {test_wasi_romfs_len},
                .layer_cnt = 1};
    const char *args = "/";
    vfs = VfsInit();
    virt1 = VfsVirtualInit(NULL, NULL);
    romfs = VfsRomfsInit(&w, args);
    VfsRegister(vfs, "/", virt1);
    VfsRegister(vfs, "rom", romfs);
}

TEST_TEAR_DOWN(vfs_openclose) { VfsDestroy(&vfs); }

TEST(vfs_openclose, OpenFail) {
    int i = VfsOpen(vfs, "xxx", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE(vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(NULL, vfs->fildes[4].drv);

    i = VfsOpen(vfs, "/roms", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
    TEST_ASSERT_FALSE(vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(NULL, vfs->fildes[4].drv);
}

TEST(vfs_openclose, OpenThenClose) {
    int i;

    i = VfsOpen(vfs, "/", 0);
    TEST_ASSERT_EQUAL_INT(3, i);
    TEST_ASSERT_TRUE(vfs->fildes[3].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[3].drv);

    i = VfsOpen(vfs, "/rom", 0);
    TEST_ASSERT_EQUAL_INT(4, i);
    TEST_ASSERT_TRUE(vfs->fildes[4].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[4].drv);

    /* this should work for app.wasm */
    i = VfsOpenAt(vfs, i, "/rom/app.wasm", 0);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_TRUE(vfs->fildes[5].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[5].drv);

    i = VfsOpen(vfs, "/", 0);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_TRUE(vfs->fildes[6].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[6].drv);

    i = VfsOpen(vfs, "/rom/app.wasm", 0);
    TEST_ASSERT_EQUAL_INT(7, i);
    TEST_ASSERT_TRUE(vfs->fildes[7].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[7].drv);

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

    i = VfsClose(vfs, 6);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(vfs->fildes[5].opened);

    i = VfsClose(vfs, 7);
    TEST_ASSERT_EQUAL_INT(0, i);
    TEST_ASSERT_FALSE(vfs->fildes[5].opened);
}

TEST_GROUP_RUNNER(vfs_openclose) {
    RUN_TEST_CASE(vfs_openclose, OpenFail);
    RUN_TEST_CASE(vfs_openclose, OpenThenClose);
}

/***************************************/
TEST_GROUP(vfs_prefix_router);
/***************************************/

/* Phase 6 — drivers are registered straight into the per-ctx DevFs/NetFs
 * tables; the legacy virt-rooted /dev and /net sub-mounts are gone. The
 * prefix router exact-matches the suffix against the registered names and
 * dispatches into the typed-FD table. */

static vfs_driver_t *router_dev_drv;
static vfs_driver_t *router_net_drv;

TEST_SETUP(vfs_prefix_router) {
    vfs = VfsInit();
    virt1 = VfsVirtualInit(NULL, NULL);
    router_dev_drv = VfsNullInit(NULL, NULL);
    router_net_drv = VfsNullInit(NULL, NULL);
    VfsRegister(vfs, "/", virt1);
    DevFs_Register(vfs, "null", router_dev_drv);
    NetFs_Register(vfs, "null", router_net_drv);
}

TEST_TEAR_DOWN(vfs_prefix_router) { VfsDestroy(&vfs); }

TEST(vfs_prefix_router, DevPathTakesTypedFdSlot) {
    int fd = VfsOpen(vfs, "/dev/null", VFS_O_RDWR);

    TEST_ASSERT_TRUE(fd >= ROOT_FD);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_DEV, vfs->fds[fd].type);
    TEST_ASSERT_NOT_NULL(vfs->fds[fd].internal_ctx);
    /* Legacy slot must remain untouched for typed FDs. */
    TEST_ASSERT_FALSE(vfs->fildes[fd].opened);
    TEST_ASSERT_NULL(vfs->fildes[fd].drv);

    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(0, VfsRead(vfs, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(3, VfsWrite(vfs, fd, "abc", 3));

    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[fd].type);
    TEST_ASSERT_NULL(vfs->fds[fd].internal_ctx);
}

TEST(vfs_prefix_router, NetPathTakesTypedFdSlot) {
    int fd = VfsOpen(vfs, "/net/null", VFS_O_RDWR);

    TEST_ASSERT_TRUE(fd >= ROOT_FD);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NET, vfs->fds[fd].type);
    TEST_ASSERT_NOT_NULL(vfs->fds[fd].internal_ctx);
    TEST_ASSERT_FALSE(vfs->fildes[fd].opened);

    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[fd].type);
}

TEST(vfs_prefix_router, NonRoutedPathStaysOnLegacy) {
    int fd = VfsOpen(vfs, "/", 0);

    TEST_ASSERT_EQUAL_INT(ROOT_FD, fd);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[fd].type);
    TEST_ASSERT_TRUE(vfs->fildes[fd].opened);
    TEST_ASSERT_EQUAL_PTR(vfs->rootDriver, vfs->fildes[fd].drv);
}

TEST(vfs_prefix_router, TypedAndLegacyFdsCoexist) {
    int dev_fd = VfsOpen(vfs, "/dev/null", VFS_O_RDWR);
    int legacy_fd = VfsOpen(vfs, "/", 0);

    TEST_ASSERT_NOT_EQUAL(dev_fd, legacy_fd);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_DEV, vfs->fds[dev_fd].type);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[legacy_fd].type);
    TEST_ASSERT_TRUE(vfs->fildes[legacy_fd].opened);

    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, dev_fd));
    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, legacy_fd));
}

TEST(vfs_prefix_router, MissingDevSuffixReturnsEnoent) {
    int fd = VfsOpen(vfs, "/dev/does-not-exist", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST(vfs_prefix_router, DevFsExactMatchOnly) {
    /* A suffix that *starts* with a registered name but extends past it must
     * not collide with the registration — DevFs lookup is exact, not prefix. */
    int fd = VfsOpen(vfs, "/dev/nullx", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST(vfs_prefix_router, DevFsRejectsDuplicateRegister) {
    vfs_driver_t *dup = VfsNullInit(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-EEXIST, DevFs_Register(vfs, "null", dup));
}

TEST_GROUP_RUNNER(vfs_prefix_router) {
    RUN_TEST_CASE(vfs_prefix_router, DevPathTakesTypedFdSlot);
    RUN_TEST_CASE(vfs_prefix_router, NetPathTakesTypedFdSlot);
    RUN_TEST_CASE(vfs_prefix_router, NonRoutedPathStaysOnLegacy);
    RUN_TEST_CASE(vfs_prefix_router, TypedAndLegacyFdsCoexist);
    RUN_TEST_CASE(vfs_prefix_router, MissingDevSuffixReturnsEnoent);
    RUN_TEST_CASE(vfs_prefix_router, DevFsExactMatchOnly);
    RUN_TEST_CASE(vfs_prefix_router, DevFsRejectsDuplicateRegister);
}
