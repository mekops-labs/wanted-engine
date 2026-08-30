/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <dummy-fs.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-tarfs.h>
#include <vfs.h>

/* VfsUnlink must resolve `path` and route by it, the way VfsOpenAt already
 * does — not by the calling fd's cached type. Exercised through the real
 * entry point a wapp's unlink() reaches, with a TarFS fd standing in for the
 * only preopen a wapp actually gets. */

TEST_GROUP(vfs_unlink_routing);

static vfs_ctx_t vfs;
static uint8_t layerBuf[512 * 2];

TEST_SETUP(vfs_unlink_routing) {
    DummyRegistryReset();
    memset(layerBuf, 0, sizeof(layerBuf));

    const char payload[] = "x";
    TarHeader(layerBuf, "app.wasm", sizeof(payload) - 1, '0');
    memcpy(layerBuf + 512, payload, sizeof(payload) - 1);

    uint8_t *layers[1] = {layerBuf};
    size_t lens[1] = {sizeof(layerBuf)};
    vfs_tarfs_ctx_t *t = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(t);

    vfs = VfsInit();
    TEST_ASSERT_NOT_NULL(vfs);
    VfsAttachTarfs(vfs, t);

    vfs_driver_t *wanted = VfsWantedInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(wanted);
    TEST_ASSERT_EQUAL_INT(0, DevFs_Register(vfs, "wanted", wanted));
}

TEST_TEAR_DOWN(vfs_unlink_routing) {
    VfsDestroy(&vfs);
    DummyRegistryReset();
}

/* The only fd a real wapp ever holds for a relative unlink is its TarFS
 * preopen — never a /dev one. The old code dispatched on that fd's cached
 * type and hit TarFS's -EROFS before `path` was ever examined. */
TEST(vfs_unlink_routing, RemovesARegistryEntryThroughTheTarfsPreopen) {
    reg_entry_t seed[1] = {MakeEntry("flasher", "0.4.1", 20480)};
    DummyRegistrySeed(seed, 1);

    int root = VfsOpen(vfs, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(root >= 0);

    TEST_ASSERT_EQUAL_INT(
        0, VfsUnlink(vfs, root, "/dev/wanted/reg/flasher:0.4.1"));

    /* Gone, not just reported gone: a second unlink of the same ref finds
     * nothing left to remove. */
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsUnlink(vfs, root, "/dev/wanted/reg/flasher:0.4.1"));

    VfsClose(vfs, root);
}

TEST(vfs_unlink_routing, UnknownEntryReportsEnoentNotErofs) {
    int root = VfsOpen(vfs, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(root >= 0);

    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsUnlink(vfs, root, "/dev/wanted/reg/nothere:1.0.0"));

    VfsClose(vfs, root);
}

/* TarFS itself must still refuse every mutation — the fix routes by path,
 * it does not make the root writable. */
TEST(vfs_unlink_routing, TarfsPathStillRefusesUnlink) {
    int root = VfsOpen(vfs, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(root >= 0);

    TEST_ASSERT_EQUAL_INT(-EROFS, VfsUnlink(vfs, root, "/app.wasm"));

    VfsClose(vfs, root);
}

TEST_GROUP_RUNNER(vfs_unlink_routing) {
    RUN_TEST_CASE(vfs_unlink_routing,
                  RemovesARegistryEntryThroughTheTarfsPreopen);
    RUN_TEST_CASE(vfs_unlink_routing, UnknownEntryReportsEnoentNotErofs);
    RUN_TEST_CASE(vfs_unlink_routing, TarfsPathStillRefusesUnlink);
}
