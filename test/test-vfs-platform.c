/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <platform.h>
#include <vfs.h>
#include <vfs-drivers.h>
#include <wasi.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_platform_driver — driver vtable exercised directly, no WASM layer
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(vfs_platform_driver);

static vfs_driver_t *drv;

TEST_SETUP(vfs_platform_driver) {
    DummyFsReset();
    drv = VfsPlatformFsInit(NULL, NULL);
}

TEST_TEAR_DOWN(vfs_platform_driver) {
    if (drv && drv->Destroy)
        drv->Destroy(drv);
    drv = NULL;
}

TEST(vfs_platform_driver, Open_CreateFile_ReturnsValidFd) {
    int fd = drv->Open(drv->ctx, "/test.txt", VFS_O_CREAT | VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    drv->Close(drv->ctx, fd);
}

TEST(vfs_platform_driver, Open_NonexistentWithoutCreat_ReturnsEnoent) {
    int fd = drv->Open(drv->ctx, "/ghost.txt", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST(vfs_platform_driver, Open_ExclOnExisting_ReturnsEexist) {
    drv->Open(drv->ctx, "/excl.txt", VFS_O_CREAT | VFS_O_RDWR);
    int fd = drv->Open(drv->ctx, "/excl.txt", VFS_O_CREAT | VFS_O_EXCL);
    TEST_ASSERT_EQUAL_INT(-EEXIST, fd);
}

TEST(vfs_platform_driver, WriteAndRead_RoundTrip) {
    int fd = drv->Open(drv->ctx, "/rw.txt", VFS_O_CREAT | VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);

    int n = drv->Write(drv->ctx, fd, "hello", 5);
    TEST_ASSERT_EQUAL_INT(5, n);

    long pos;
    TEST_ASSERT_EQUAL_INT(0, drv->Seek(drv->ctx, fd, 0, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(0, pos);

    char buf[16] = {0};
    n = drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(5, n);
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, 5);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_platform_driver, Stat_File) {
    int fd = drv->Open(drv->ctx, "/stat.txt", VFS_O_CREAT | VFS_O_RDWR);
    TEST_ASSERT_TRUE(fd >= 0);
    drv->Write(drv->ctx, fd, "abc", 3);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(3, st.size);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_platform_driver, Stat_Dir) {
    int fd = drv->Open(drv->ctx, "/mydir", VFS_O_CREAT | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_platform_driver, Seek_SetCurEnd) {
    int fd = drv->Open(drv->ctx, "/seek.txt", VFS_O_CREAT | VFS_O_RDWR);
    drv->Write(drv->ctx, fd, "0123456789", 10);

    long pos;
    drv->Seek(drv->ctx, fd, 4, VFS_SEEK_SET, &pos);
    TEST_ASSERT_EQUAL_INT(4, pos);

    char buf[4] = {0};
    int n = drv->Read(drv->ctx, fd, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("456", buf, 3);

    drv->Seek(drv->ctx, fd, 0, VFS_SEEK_END, &pos);
    TEST_ASSERT_EQUAL_INT(10, pos);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_platform_driver, Close_InvalidFd_ReturnsEbadf) {
    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Close(drv->ctx, 42));
}

TEST(vfs_platform_driver, ReadDir_ListsDirectChildren) {
    int dir_fd = drv->Open(drv->ctx, "/rddir", VFS_O_CREAT | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(dir_fd >= 0);

    int f1 = drv->Open(drv->ctx, "/rddir/a.txt", VFS_O_CREAT | VFS_O_RDWR);
    int f2 = drv->Open(drv->ctx, "/rddir/b.txt", VFS_O_CREAT | VFS_O_RDWR);
    /* Nested path should NOT appear in rddir listing */
    int sub = drv->Open(drv->ctx, "/rddir/sub", VFS_O_CREAT | VFS_O_DIRECTORY);
    drv->Open(drv->ctx, "/rddir/sub/nested.txt", VFS_O_CREAT | VFS_O_RDWR);
    drv->Close(drv->ctx, f1);
    drv->Close(drv->ctx, f2);
    drv->Close(drv->ctx, sub);

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, drv->ReadDir(drv->ctx, dir_fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "a.txt", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "b.txt", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "sub",   3));
    /* nested.txt is grandchild — must not appear */
    TEST_ASSERT_FALSE(HasBytes(buf, used, "nested.txt", 10));

    drv->Close(drv->ctx, dir_fd);
}

TEST(vfs_platform_driver, Rename_MovesFile) {
    int dir_fd = drv->Open(drv->ctx, "/rendir", VFS_O_CREAT | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(dir_fd >= 0);

    int f = drv->Open(drv->ctx, "/rendir/old.txt", VFS_O_CREAT | VFS_O_RDWR);
    drv->Write(drv->ctx, f, "data", 4);
    drv->Close(drv->ctx, f);

    int ret = drv->Rename(drv->ctx, dir_fd, "old.txt", dir_fd, "new.txt");
    TEST_ASSERT_EQUAL_INT(0, ret);

    TEST_ASSERT_EQUAL_INT(-ENOENT,
        drv->Open(drv->ctx, "/rendir/old.txt", VFS_O_RDONLY));

    int new_fd = drv->Open(drv->ctx, "/rendir/new.txt", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(new_fd >= 0);
    drv->Close(drv->ctx, new_fd);
    drv->Close(drv->ctx, dir_fd);
}

TEST(vfs_platform_driver, Mkdir_CreatesSubdir) {
    int dir_fd = drv->Open(drv->ctx, "/mktest", VFS_O_CREAT | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(dir_fd >= 0);

    TEST_ASSERT_EQUAL_INT(0, drv->Mkdir(drv->ctx, dir_fd, "sub"));

    int sub_fd = drv->Open(drv->ctx, "/mktest/sub", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(sub_fd >= 0);

    vfs_stat_t st;
    drv->Stat(drv->ctx, sub_fd, &st);
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    drv->Close(drv->ctx, sub_fd);
    drv->Close(drv->ctx, dir_fd);
}

TEST(vfs_platform_driver, Mkdir_DuplicateReturnsEexist) {
    int dir_fd = drv->Open(drv->ctx, "/dupdir", VFS_O_CREAT | VFS_O_DIRECTORY);
    drv->Mkdir(drv->ctx, dir_fd, "sub");
    TEST_ASSERT_EQUAL_INT(-EEXIST, drv->Mkdir(drv->ctx, dir_fd, "sub"));
    drv->Close(drv->ctx, dir_fd);
}

TEST_GROUP_RUNNER(vfs_platform_driver) {
    RUN_TEST_CASE(vfs_platform_driver, Open_CreateFile_ReturnsValidFd);
    RUN_TEST_CASE(vfs_platform_driver, Open_NonexistentWithoutCreat_ReturnsEnoent);
    RUN_TEST_CASE(vfs_platform_driver, Open_ExclOnExisting_ReturnsEexist);
    RUN_TEST_CASE(vfs_platform_driver, WriteAndRead_RoundTrip);
    RUN_TEST_CASE(vfs_platform_driver, Stat_File);
    RUN_TEST_CASE(vfs_platform_driver, Stat_Dir);
    RUN_TEST_CASE(vfs_platform_driver, Seek_SetCurEnd);
    RUN_TEST_CASE(vfs_platform_driver, Close_InvalidFd_ReturnsEbadf);
    RUN_TEST_CASE(vfs_platform_driver, ReadDir_ListsDirectChildren);
    RUN_TEST_CASE(vfs_platform_driver, Rename_MovesFile);
    RUN_TEST_CASE(vfs_platform_driver, Mkdir_CreatesSubdir);
    RUN_TEST_CASE(vfs_platform_driver, Mkdir_DuplicateReturnsEexist);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * wasi_preopen_fs — VfsRename / VfsMkdir through a preopen-backed VFS
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(wasi_preopen_fs);

static wasi_ctx_t *wctx;
static vfs_ctx_t   vfs;

TEST_SETUP(wasi_preopen_fs) {
    DummyFsReset();
    vfs  = VfsInit();
    wctx = InitWasiContext();
    wctx->vfsCtx = vfs;
}

TEST_TEAR_DOWN(wasi_preopen_fs) {
    FreeWasiContext(wctx);
    VfsDestroy(&vfs);
}

TEST(wasi_preopen_fs, AddPreopen_BindsToVfs) {
    int host_fd = PlatformOpenStateDir("/var/lib/test");
    TEST_ASSERT_TRUE(host_fd >= 0);

    uint8_t before = wctx->preopens_cnt;
    TEST_ASSERT_EQUAL_INT(0, WasiCtxAddPreopen(wctx, "/var/lib/test", host_fd));
    TEST_ASSERT_EQUAL_UINT8(before + 1, wctx->preopens_cnt);
    TEST_ASSERT_TRUE(wctx->preopens[wctx->preopens_cnt - 1].fd >= 0);
}

TEST(wasi_preopen_fs, OpenAt_CreateAndRead) {
    int host_fd = PlatformOpenStateDir("/state");
    TEST_ASSERT_EQUAL_INT(0, WasiCtxAddPreopen(wctx, "/state", host_fd));
    int vfs_fd = wctx->preopens[wctx->preopens_cnt - 1].fd;
    TEST_ASSERT_TRUE(vfs_fd >= 0);

    int f = VfsOpenAt(vfs, vfs_fd, "cfg.json", VFS_O_CREAT | VFS_O_RDWR);
    TEST_ASSERT_TRUE(f >= 0);

    int n = VfsWrite(vfs, f, "{}", 2);
    TEST_ASSERT_EQUAL_INT(2, n);

    long pos;
    VfsSeek(vfs, f, 0, VFS_SEEK_SET, &pos);

    char buf[8] = {0};
    n = VfsRead(vfs, f, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING_LEN("{}", buf, 2);

    VfsClose(vfs, f);
}

TEST(wasi_preopen_fs, Mkdir_CreatesDirectory) {
    int host_fd = PlatformOpenStateDir("/var/lib/sheriff");
    TEST_ASSERT_EQUAL_INT(0, WasiCtxAddPreopen(wctx, "/var/lib/sheriff", host_fd));
    int vfs_fd = wctx->preopens[wctx->preopens_cnt - 1].fd;
    TEST_ASSERT_TRUE(vfs_fd >= 0);

    int ret = VfsMkdir(vfs, vfs_fd, "cache");
    TEST_ASSERT_EQUAL_INT(0, ret);

    int sub = VfsOpenAt(vfs, vfs_fd, "cache", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_TRUE(sub >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, sub, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    VfsClose(vfs, sub);
}

TEST(wasi_preopen_fs, Rename_MovesFileInPreopen) {
    int host_fd = PlatformOpenStateDir("/var/lib/rename");
    TEST_ASSERT_EQUAL_INT(0, WasiCtxAddPreopen(wctx, "/var/lib/rename", host_fd));
    int vfs_fd = wctx->preopens[wctx->preopens_cnt - 1].fd;
    TEST_ASSERT_TRUE(vfs_fd >= 0);

    int f = VfsOpenAt(vfs, vfs_fd, "desired.json.tmp",
                      VFS_O_CREAT | VFS_O_RDWR);
    TEST_ASSERT_TRUE(f >= 0);
    VfsWrite(vfs, f, "{\"generation\":1}", 16);
    VfsClose(vfs, f);

    int ret = VfsRename(vfs, vfs_fd, "desired.json.tmp",
                        vfs_fd, "desired.json");
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* tmp path gone */
    TEST_ASSERT_EQUAL_INT(-ENOENT,
        VfsOpenAt(vfs, vfs_fd, "desired.json.tmp", VFS_O_RDONLY));

    /* target path exists and preserves content */
    int final = VfsOpenAt(vfs, vfs_fd, "desired.json", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(final >= 0);

    char buf[32] = {0};
    int n = VfsRead(vfs, final, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(16, n);
    TEST_ASSERT_EQUAL_STRING_LEN("{\"generation\":1}", buf, 16);

    VfsClose(vfs, final);
}

TEST_GROUP_RUNNER(wasi_preopen_fs) {
    RUN_TEST_CASE(wasi_preopen_fs, AddPreopen_BindsToVfs);
    RUN_TEST_CASE(wasi_preopen_fs, OpenAt_CreateAndRead);
    RUN_TEST_CASE(wasi_preopen_fs, Mkdir_CreatesDirectory);
    RUN_TEST_CASE(wasi_preopen_fs, Rename_MovesFileInPreopen);
}
