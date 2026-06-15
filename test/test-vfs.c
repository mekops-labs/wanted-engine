/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-netfs.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

static vfs_ctx_t vfs;
static vfs_driver_t *router_dev_drv;
static vfs_driver_t *router_net_drv;

/***************************************/
TEST_GROUP(vfs_init);
/***************************************/

TEST_SETUP(vfs_init) {}

TEST_TEAR_DOWN(vfs_init) {}

TEST(vfs_init, InitAndDestroy) {
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_NOT_NULL(c);

    /* ctx is zero-initialised; the typed FD table starts empty. */
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, c->fds[i].type);
    }
    TEST_ASSERT_NULL(c->tarfs);
    TEST_ASSERT_EQUAL_UINT8(0, c->devfs_cnt);
    TEST_ASSERT_EQUAL_UINT8(0, c->netfs_cnt);

    VfsDestroy(&c);
    TEST_ASSERT_NULL(c);
}

TEST_GROUP_RUNNER(vfs_init) { RUN_TEST_CASE(vfs_init, InitAndDestroy); }

/***************************************/
TEST_GROUP(vfs_prefix_router);
/***************************************/

/* The only open sinks are DevFs, NetFs, TARFS, and stdio STREAM slots. The
 * prefix router exact-matches the suffix and dispatches into the typed-FD
 * table; everything else returns -ENOENT. */

TEST_SETUP(vfs_prefix_router) {
    vfs = VfsInit();
    router_dev_drv = VfsNullInit(NULL, NULL);
    router_net_drv = VfsNullInit(NULL, NULL);
    DevFs_Register(vfs, "null", router_dev_drv);
    NetFs_Register(vfs, "null", router_net_drv);
}

TEST_TEAR_DOWN(vfs_prefix_router) { VfsDestroy(&vfs); }

TEST(vfs_prefix_router, DevPathTakesTypedFdSlot) {
    int fd = VfsOpen(vfs, "/dev/null", VFS_O_RDWR);

    TEST_ASSERT_TRUE(fd >= ROOT_FD);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_DEV, vfs->fds[fd].type);
    TEST_ASSERT_NOT_NULL(vfs->fds[fd].internal_ctx);

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

    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[fd].type);
}

/* A net driver whose Open has a side effect (here: counts calls and fails like
 * a socket node on a netless build). Used to prove stat never opens the node.
 */
static int g_boom_open_calls;
static int BoomOpen(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)d;
    (void)path;
    (void)flags;
    g_boom_open_calls++;
    return -ECONNABORTED;
}
static vfs_driver_t boom_drv = {
    .filetype = VFS_FILETYPE_SOCKET_STREAM,
    .Open = BoomOpen,
};

TEST(vfs_prefix_router, NetStatDoesNotOpenSocket) {
    g_boom_open_calls = 0;
    TEST_ASSERT_EQUAL_INT(0, NetFs_Register(vfs, "boom", &boom_drv));

    int base = VfsOpen(vfs, "/net", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(base >= ROOT_FD);

    /* Stat reports the driver's filetype without invoking Open. */
    vfs_stat_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_INT(0, VfsStatAt(vfs, base, "/net/boom", &st));
    TEST_ASSERT_EQUAL_INT(VFS_FILETYPE_SOCKET_STREAM, st.filetype);
    TEST_ASSERT_EQUAL_INT(0, g_boom_open_calls);

    /* A real open still routes into the driver (creating the socket). */
    TEST_ASSERT_EQUAL_INT(-ECONNABORTED, VfsOpen(vfs, "/net/boom", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(1, g_boom_open_calls);

    VfsClose(vfs, base);
}

TEST(vfs_prefix_router, NonRoutedPathReturnsEnoent) {
    /* Without a tarfs context attached, root-relative paths have no sink. */
    int fd = VfsOpen(vfs, "/", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);

    fd = VfsOpen(vfs, "/some/file", 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
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
    RUN_TEST_CASE(vfs_prefix_router, NetStatDoesNotOpenSocket);
    RUN_TEST_CASE(vfs_prefix_router, NonRoutedPathReturnsEnoent);
    RUN_TEST_CASE(vfs_prefix_router, MissingDevSuffixReturnsEnoent);
    RUN_TEST_CASE(vfs_prefix_router, DevFsExactMatchOnly);
    RUN_TEST_CASE(vfs_prefix_router, DevFsRejectsDuplicateRegister);
}

/***************************************/
TEST_GROUP(vfs_stream_register);
/***************************************/

/* VfsRegister is the stdio-only path. Each `<stdin>`/`<stdout>`/`<stderr>`
 * mount drops the supplied driver into a STREAM slot at fd 0/1/2; VfsDestroy
 * walks those slots and frees the driver. Anything else is destroyed and
 * returns -EINVAL. */

TEST_SETUP(vfs_stream_register) { vfs = VfsInit(); }

TEST_TEAR_DOWN(vfs_stream_register) { VfsDestroy(&vfs); }

TEST(vfs_stream_register, StdioSlotsClaimed) {
    vfs_driver_t *in = VfsNullInit(NULL, NULL);
    vfs_driver_t *out = VfsNullInit(NULL, NULL);
    vfs_driver_t *err = VfsNullInit(NULL, NULL);

    TEST_ASSERT_EQUAL_INT(0, VfsRegister(vfs, "<stdin>", in));
    TEST_ASSERT_EQUAL_INT(0, VfsRegister(vfs, "<stdout>", out));
    TEST_ASSERT_EQUAL_INT(0, VfsRegister(vfs, "<stderr>", err));

    TEST_ASSERT_EQUAL_INT(VFS_TYPE_STREAM, vfs->fds[VFS_STDIN].type);
    TEST_ASSERT_EQUAL_PTR(in, vfs->fds[VFS_STDIN].driver);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_STREAM, vfs->fds[VFS_STDOUT].type);
    TEST_ASSERT_EQUAL_PTR(out, vfs->fds[VFS_STDOUT].driver);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_STREAM, vfs->fds[VFS_STDERR].type);
    TEST_ASSERT_EQUAL_PTR(err, vfs->fds[VFS_STDERR].driver);
}

TEST(vfs_stream_register, ArbitraryPathRejected) {
    vfs_driver_t *drv = VfsNullInit(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsRegister(vfs, "/", drv));
    /* Driver pointer is now invalid (Destroy was called) — slot stays empty. */
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_NONE, vfs->fds[ROOT_FD].type);
}

TEST_GROUP_RUNNER(vfs_stream_register) {
    RUN_TEST_CASE(vfs_stream_register, StdioSlotsClaimed);
    RUN_TEST_CASE(vfs_stream_register, ArbitraryPathRejected);
}
