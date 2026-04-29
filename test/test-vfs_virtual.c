#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <vfs/vfs-virtual.h>
#include <wanted-api.h>

static int dummyOpened;
static int dummyClosed;
static int dummyRead;
static int dummyWrite;
static int dummySeek;

static int _DummyOpen(vfs_driver_ctx_t d, const char *path,
                      vfs_oflags_t flags) {
    if (memcmp("/", path, 2) != 0) {
        return -EPERM;
    }
    dummyOpened++;
    return 0;
}

static int _DummyClose(vfs_driver_ctx_t d, int fd) {
    dummyClosed++;
    return 0;
}

static int _DummyRead(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    dummyRead++;
    return 0;
}

static int _DummyWrite(vfs_driver_ctx_t d, int fd, const void *buf,
                       size_t nbyte) {
    dummyWrite++;
    return 0;
}

static int _DummySeek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                      long *pos) {
    dummySeek++;
    return 0;
}

static int _DummyStat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    s->filetype = VFS_FILETYPE_REGULAR_FILE;
    s->dev = 1234;
    s->ino = 1;

    return 0;
}

static vfs_driver_t *virt;
static vfs_driver_t *virt2;

static vfs_driver_t dummy_test = {
    .Open = _DummyOpen,
    .Close = _DummyClose,
    .Read = _DummyRead,
    .Write = _DummyWrite,
    .Seek = _DummySeek,
};

/***************************************/
TEST_GROUP(vfs_virtual_init);
/***************************************/

TEST_SETUP(vfs_virtual_init) {}

TEST_TEAR_DOWN(vfs_virtual_init) {}

TEST(vfs_virtual_init, InitAndDestroy) {
    int ret;
    vfs_driver_t *v;

    v = VfsVirtualInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(v);

    vfs_entry_t *fs = v->ctx->entries;

    TEST_ASSERT_EQUAL_MEMORY("Virt", v->id, 4);

    TEST_ASSERT_EQUAL_PTR(v, fs[0].drv);
    TEST_ASSERT_EQUAL_STRING("/", fs[0].name);

    v->Destroy(v);
}

TEST_GROUP_RUNNER(vfs_virtual_init) {
    RUN_TEST_CASE(vfs_virtual_init, InitAndDestroy);
}

/***************************************/
TEST_GROUP(vfs_virtual_register);
/***************************************/

TEST_SETUP(vfs_virtual_register) { virt = VfsVirtualInit(NULL, NULL); }

TEST_TEAR_DOWN(vfs_virtual_register) { virt->Destroy(virt); }

TEST(vfs_virtual_register, RegisterFail) {
    int ret;
    vfs_driver_t dummy = {0};
    vfs_entry_t *fs = virt->ctx->entries;

    ret = TRY_DRV(virt, Register, "", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    TEST_ASSERT_NULL(fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("", fs[1].name);

    ret = TRY_DRV(virt, Register, "..", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    ret = TRY_DRV(virt, Register, ".", &dummy);
    TEST_ASSERT_EQUAL(-EINVAL, ret);

    for (int i = 0; i < MAX_ENTRIES; i++) {
        char d[2] = {'0' + i, '\0'};
        ret = TRY_DRV(virt, Register, d, &dummy);
    }
    TEST_ASSERT_EQUAL(-ENFILE, ret);
}

TEST(vfs_virtual_register, RegisterSingle) {
    int ret;
    vfs_driver_t dummy = {0};
    vfs_entry_t *fs = virt->ctx->entries;

    ret = TRY_DRV(virt, Register, "a", &dummy);
    TEST_ASSERT_EQUAL(1, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("a", fs[1].name);
}

TEST(vfs_virtual_register, RegisterMultiple) {
    int ret;
    vfs_driver_t dummy1 = {0};
    vfs_driver_t dummy2 = {0};
    vfs_entry_t *fs = virt->ctx->entries;

    ret = TRY_DRV(virt, Register, "abc", &dummy1);
    TEST_ASSERT_EQUAL(1, ret);

    ret = TRY_DRV(virt, Register, "ab", &dummy2);
    TEST_ASSERT_EQUAL(2, ret);

    TEST_ASSERT_EQUAL_PTR(&dummy1, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("abc", fs[1].name);

    TEST_ASSERT_EQUAL_PTR(&dummy2, fs[2].drv);
    TEST_ASSERT_EQUAL_STRING("ab", fs[2].name);

    TEST_ASSERT_NULL(fs[3].drv);
}

TEST(vfs_virtual_register, RegisterTree) {
    int ret;
    vfs_driver_t dummy = {0};

    virt2 = VfsVirtualInit(NULL, NULL);

    ret = TRY_DRV(virt, Register, "a", virt2);
    TEST_ASSERT_EQUAL(1, ret);

    ret = TRY_DRV(virt, Register, "a/b", &dummy);
    TEST_ASSERT_EQUAL(1, ret);

    vfs_entry_t *fs = virt2->ctx->entries;
    TEST_ASSERT_EQUAL_PTR(&dummy, fs[1].drv);
    TEST_ASSERT_EQUAL_STRING("b", fs[1].name);

    ret = TRY_DRV(virt, Register, "a/b/c", &dummy);
    TEST_ASSERT_EQUAL(-EPERM, ret);
}

TEST_GROUP_RUNNER(vfs_virtual_register) {
    RUN_TEST_CASE(vfs_virtual_register, RegisterFail);
    RUN_TEST_CASE(vfs_virtual_register, RegisterSingle);
    RUN_TEST_CASE(vfs_virtual_register, RegisterMultiple);
    RUN_TEST_CASE(vfs_virtual_register, RegisterTree);
}

/***************************************/
TEST_GROUP(vfs_virtual_find);
/***************************************/

TEST_SETUP(vfs_virtual_find) {
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "b", &dummy_test);
    TRY_DRV(virt, Register, "c", &dummy_test);
    TRY_DRV(virt, Register, "net", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(virt, Register, "dev", virt2);
    TRY_DRV(virt, Register, "dev/a", &dummy_test);
    TRY_DRV(virt, Register, ".dotfile", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_find) { virt->Destroy(virt); }

TEST(vfs_virtual_find, findFileNotFound) {
    int i;
    vfs_entry_t *fs = virt->ctx->entries;
    const char *pathLeft;

    i = VfsFindEntry("..", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("/not_a_file", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("/n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("n", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("aa", fs, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);

    i = VfsFindEntry("../dev/xyz", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual_find, findFileRoot) {
    vfs_entry_t *fs = virt->ctx->entries;

    int i = VfsFindEntry("/", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntry(".", fs, NULL);
    TEST_ASSERT_EQUAL_INT(0, i);

    i = VfsFindEntry(".dotfile", fs, NULL);
    TEST_ASSERT_EQUAL_INT(7, i);

    i = VfsFindEntry("/c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntry("c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);
}

TEST(vfs_virtual_find, findFileDir) {
    vfs_entry_t *fs = virt->ctx->entries;
    vfs_entry_t *fs2 = virt2->ctx->entries;
    const char *pathLeft;

    int i = VfsFindEntry("c", fs, NULL);
    TEST_ASSERT_EQUAL_INT(3, i);

    i = VfsFindEntry("dir/xyz", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("xyz", pathLeft);

    i = VfsFindEntry(pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(1, i);

    i = VfsFindEntry("dir/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("a", pathLeft);

    i = VfsFindEntry(pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntry("dev/a", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING("a", pathLeft);

    i = VfsFindEntry(pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(2, i);

    i = VfsFindEntry("dev/aa", fs, &pathLeft);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING("aa", pathLeft);

    i = VfsFindEntry(pathLeft, fs2, NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT, i);
}

TEST(vfs_virtual_find, findFileDriver) {
    vfs_entry_t *fs = virt->ctx->entries;

    const char *drvPath = NULL;

    int i = VfsFindEntry("/dev/a", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING("a", drvPath);
    TEST_ASSERT_EQUAL_PTR(virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntry("/dev/.", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(6, i);
    TEST_ASSERT_EQUAL_STRING(".", drvPath);
    TEST_ASSERT_EQUAL_PTR(virt2, fs[i].drv);

    drvPath = NULL;

    i = VfsFindEntry("/dir/x/y/z", fs, &drvPath);
    TEST_ASSERT_NOT_NULL(drvPath);
    TEST_ASSERT_EQUAL_INT(5, i);
    TEST_ASSERT_EQUAL_STRING("x/y/z", drvPath);
    TEST_ASSERT_EQUAL_PTR(virt2, fs[i].drv);

    drvPath = NULL;
}

TEST_GROUP_RUNNER(vfs_virtual_find) {
    RUN_TEST_CASE(vfs_virtual_find, findFileNotFound);
    RUN_TEST_CASE(vfs_virtual_find, findFileRoot);
    RUN_TEST_CASE(vfs_virtual_find, findFileDir);
    RUN_TEST_CASE(vfs_virtual_find, findFileDriver);
}

/***************************************/
TEST_GROUP(vfs_virtual_open);
/***************************************/

TEST_SETUP(vfs_virtual_open) {
    dummyOpened = 0;
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "b", &dummy_test);
    TRY_DRV(virt, Register, "c", &dummy_test);
    TRY_DRV(virt, Register, "net", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(virt, Register, "dev", virt2);
    TRY_DRV(virt, Register, "dev/a", &dummy_test);
    TRY_DRV(virt, Register, ".dotfile", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_open) { virt->Destroy(virt); }

TEST(vfs_virtual_open, OpenFail) {
    int r;
    char path_too_long[MAX_PATH_LEN + 1];

    r = TRY_DRV(virt, Open, "", 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Open, NULL, 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Open, "..", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(virt, Open, "x", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(virt, OpenAt, MAX_ENTRIES + 1, "a", 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, OpenAt, MAX_ENTRIES + 1, "a", 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    memset(path_too_long, 'a', sizeof(path_too_long));
    path_too_long[MAX_PATH_LEN] = '\0';

    r = TRY_DRV(virt, OpenAt, 0, path_too_long, 0);
    TEST_ASSERT_EQUAL(-ENAMETOOLONG, r);

    r = TRY_DRV(virt, OpenAt, 0, "a/b", 0);
    TEST_ASSERT_EQUAL(-EPERM, r);

    r = TRY_DRV(virt, OpenAt, 0, "dir/b", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(virt, OpenAt, 0, "dir/../x", 0);
    TEST_ASSERT_EQUAL(-ENOENT, r);

    r = TRY_DRV(virt, OpenAt, 1, "dir", 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    for (int i = 0; i < MAX_OPEN; i++) {
        r = TRY_DRV(virt, Open, "dir/xyz", 0);
    }
    TEST_ASSERT_EQUAL(-EMFILE, r);
}

TEST(vfs_virtual_open, OpenSimple) {
    int r;

    r = TRY_DRV(virt, Open, "/", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Open, ".", 0);
    TEST_ASSERT_EQUAL(2, r);

    r = TRY_DRV(virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(3, r);
    TEST_ASSERT_EQUAL(1, dummyOpened);

    r = TRY_DRV(virt, Open, "a/", 0);
    TEST_ASSERT_EQUAL(4, r);
    TEST_ASSERT_EQUAL(2, dummyOpened);

    r = TRY_DRV(virt, Open, "dir", 0);
    TEST_ASSERT_EQUAL(5, r);

    r = TRY_DRV(virt, Open, ".dotfile", 0);
    TEST_ASSERT_EQUAL(6, r);

    r = TRY_DRV(virt, Open, "/.dotfile", 0);
    TEST_ASSERT_EQUAL(7, r);

    TEST_ASSERT_EQUAL(4, dummyOpened);
}

TEST(vfs_virtual_open, OpenAdvanced) {
    int r;

    r = TRY_DRV(virt, Open, "dir/xyz", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Open, "dir/../dev", 0);
    TEST_ASSERT_EQUAL(2, r);

    r = TRY_DRV(virt, Open, "/./////dir///../dev/////", 0);
    TEST_ASSERT_EQUAL(3, r);

    r = TRY_DRV(virt, Open, "dir/../dev/xyz", 0);
    TEST_ASSERT_EQUAL(4, r);

    TEST_ASSERT_EQUAL(2, dummyOpened);
}

TEST_GROUP_RUNNER(vfs_virtual_open) {
    RUN_TEST_CASE(vfs_virtual_open, OpenFail);
    RUN_TEST_CASE(vfs_virtual_open, OpenSimple);
    RUN_TEST_CASE(vfs_virtual_open, OpenAdvanced);
}

/***************************************/
TEST_GROUP(vfs_virtual_close);
/***************************************/

TEST_SETUP(vfs_virtual_close) {
    dummyClosed = 0;
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/a", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_close) { virt->Destroy(virt); }

TEST(vfs_virtual_close, CloseFail) {
    int r;

    r = TRY_DRV(virt, Close, 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Close, r + 1);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(2, r);

    r = TRY_DRV(virt, Close, 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(3, r);

    TRY_DRV(virt, Close, r);
    r = TRY_DRV(virt, Close, r);
    TEST_ASSERT_EQUAL(-EBADF, r);
}

TEST(vfs_virtual_close, CloseOk) {
    int r;

    r = TRY_DRV(virt, Open, "dir", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Close, r);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(virt, Open, "dir", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Close, r);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(virt, Open, "dir/a", 0);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Close, r);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(virt, Open, "dir/a", 0);
    TEST_ASSERT_EQUAL(1, r);

    TEST_ASSERT_EQUAL(1, dummyClosed);
}

TEST_GROUP_RUNNER(vfs_virtual_close) {
    RUN_TEST_CASE(vfs_virtual_close, CloseFail);
    RUN_TEST_CASE(vfs_virtual_close, CloseOk);
}

/***************************************/
TEST_GROUP(vfs_virtual_stat);
/***************************************/

TEST_SETUP(vfs_virtual_stat) {
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    dummy_test.Stat = _DummyStat;
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "b", &dummy_test);
    TRY_DRV(virt, Register, "c", &dummy_test);
    TRY_DRV(virt, Register, "net", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/xyz", &dummy_test);
    TRY_DRV(virt, Register, "dev", virt2);
    TRY_DRV(virt, Register, "dev/a", &dummy_test);
    TRY_DRV(virt, Register, ".dotfile", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_stat) { virt->Destroy(virt); }

TEST(vfs_virtual_stat, StatFail) {
    int r;
    vfs_stat_t stat;

    r = TRY_DRV(virt, Stat, 0, NULL);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Stat, 1, &stat);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Open, "a", 0);
    TEST_ASSERT_EQUAL(1, r);
}

TEST(vfs_virtual_stat, StatOk) {
    int r;
    vfs_stat_t stat;

    r = TRY_DRV(virt, Open, "dir", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL(1, r);

    r = TRY_DRV(virt, Stat, r, &stat);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_DIRECTORY, stat.filetype);
    TEST_ASSERT_EQUAL(VFS_O_WRONLY, stat.oflags);
    TEST_ASSERT_EQUAL_MEMORY("Virt", &stat.dev, 4);
    TEST_ASSERT_EQUAL(0, stat.ino);

    r = TRY_DRV(virt, Open, "a", VFS_O_APPEND);
    TEST_ASSERT_EQUAL(2, r);

    r = TRY_DRV(virt, Stat, r, &stat);
    TEST_ASSERT_EQUAL(VFS_FILETYPE_REGULAR_FILE, stat.filetype);
    TEST_ASSERT_EQUAL(1234, stat.dev);
    TEST_ASSERT_EQUAL(1, stat.ino);
}

TEST_GROUP_RUNNER(vfs_virtual_stat) {
    RUN_TEST_CASE(vfs_virtual_stat, StatFail);
    RUN_TEST_CASE(vfs_virtual_stat, StatOk);
}

/***************************************/
TEST_GROUP(vfs_virtual_read_write_seek);
/***************************************/

TEST_SETUP(vfs_virtual_read_write_seek) {
    dummyRead = 0;
    dummyWrite = 0;
    dummySeek = 0;
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/b", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_read_write_seek) { virt->Destroy(virt); }

TEST(vfs_virtual_read_write_seek, ReadWriteSeekFail) {
    int r;
    uint8_t buf[1];
    long pos;

    int f = TRY_DRV(virt, Open, "a", 0);

    r = TRY_DRV(virt, Read, 0, NULL, 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Read, f + 1, buf, 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Read, 0, buf, 0);
    TEST_ASSERT_EQUAL(-EISDIR, r);

    r = TRY_DRV(virt, Write, 0, NULL, 0);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Write, f + 1, buf, 0);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Write, 0, buf, 0);
    TEST_ASSERT_EQUAL(-EISDIR, r);

    r = TRY_DRV(virt, Seek, 0, 0, 0, NULL);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Seek, 0, 0, VFS_SEEK_END + 1, &pos);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, Seek, f + 1, 0, 0, &pos);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Seek, 0, 0, 0, &pos);
    TEST_ASSERT_EQUAL(-EISDIR, r);
}

TEST(vfs_virtual_read_write_seek, ReadWriteSeekOk) {
    int r;
    uint8_t buf[1];
    long pos;

    int f = TRY_DRV(virt, Open, "a", 0);

    r = TRY_DRV(virt, Read, f, buf, 0);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(virt, Write, f, buf, 0);
    TEST_ASSERT_EQUAL(0, r);

    r = TRY_DRV(virt, Seek, f, 0, 0, &pos);
    TEST_ASSERT_EQUAL(0, r);

    TEST_ASSERT_EQUAL(1, dummyRead);
    TEST_ASSERT_EQUAL(1, dummyWrite);
    TEST_ASSERT_EQUAL(1, dummySeek);
}

TEST_GROUP_RUNNER(vfs_virtual_read_write_seek) {
    RUN_TEST_CASE(vfs_virtual_read_write_seek, ReadWriteSeekFail);
    RUN_TEST_CASE(vfs_virtual_read_write_seek, ReadWriteSeekOk);
}

/***************************************/
TEST_GROUP(vfs_virtual_readdir);
/***************************************/

TEST_SETUP(vfs_virtual_readdir) {
    dummyClosed = 0;
    dummy_test.filetype = VFS_FILETYPE_REGULAR_FILE;
    virt = VfsVirtualInit(NULL, NULL);
    virt2 = VfsVirtualInit(NULL, NULL);
    TRY_DRV(virt, Register, "a", &dummy_test);
    TRY_DRV(virt, Register, "dir", virt2);
    TRY_DRV(virt, Register, "dir/b", &dummy_test);
}

TEST_TEAR_DOWN(vfs_virtual_readdir) { virt->Destroy(virt); }

#define BUF_LEN 256

TEST(vfs_virtual_readdir, ReadDirFail) {
    int r;
    uint8_t buf[BUF_LEN];
    uint64_t cookie;
    size_t bufUsed;

    r = TRY_DRV(virt, ReadDir, 0, NULL, 1, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, ReadDir, 0, buf, 1, NULL, &bufUsed);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, ReadDir, 0, buf, 1, &cookie, NULL);
    TEST_ASSERT_EQUAL(-EINVAL, r);

    r = TRY_DRV(virt, ReadDir, 1, buf, 0, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(-EBADF, r);

    r = TRY_DRV(virt, Open, "a", 0);
    r = TRY_DRV(virt, ReadDir, r, buf, BUF_LEN, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(-EPERM, r);
}

TEST(vfs_virtual_readdir, ReadDirOk) {
    int r;
    uint8_t buf[BUF_LEN];
    uint64_t cookie = 0;
    size_t bufUsed = 0;

    r = TRY_DRV(virt, ReadDir, 0, buf, 0, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(0, bufUsed);
    TEST_ASSERT_EQUAL(1, cookie);

    cookie = 0;

    r = TRY_DRV(virt, ReadDir, 0, buf, BUF_LEN, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(52, bufUsed);
    TEST_ASSERT_EQUAL(2, cookie);

    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)*buf);          // uint64_t d_next
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t) * (buf + 8));  // uint64_t d_ino
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t) * (buf + 16)); // uint32_t d_namlen
    TEST_ASSERT_EQUAL_UINT32(VFS_FILETYPE_REGULAR_FILE,
                             (uint32_t) * (buf + 20)); // vfs_filetype_t d_type
    TEST_ASSERT_EQUAL_STRING_LEN("a", buf + 24, 1);    // filename

    TEST_ASSERT_EQUAL_UINT64(2, (uint64_t) * (buf + 25)); // uint64_t d_next
    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t) * (buf + 33)); // uint64_t d_ino
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t) * (buf + 41)); // uint32_t d_namlen
    TEST_ASSERT_EQUAL_UINT32(VFS_FILETYPE_DIRECTORY,
                             (uint32_t) * (buf + 45)); // vfs_filetype_t d_type
    TEST_ASSERT_EQUAL_STRING_LEN("dir", buf + 49, 3);  // filename
}

TEST(vfs_virtual_readdir, ReadDirTwice) {
    int r;
    uint8_t buf[BUF_LEN];
    uint64_t cookie = 0;
    size_t bufUsed = 0;

    r = TRY_DRV(virt, ReadDir, 0, buf, 0, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(0, bufUsed);
    TEST_ASSERT_EQUAL(1, cookie);

    cookie = 0;

    r = TRY_DRV(virt, ReadDir, 0, buf, 40, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(40, bufUsed);
    TEST_ASSERT_EQUAL(2, cookie);

    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)*buf);          // uint64_t d_next
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t) * (buf + 8));  // uint64_t d_ino
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t) * (buf + 16)); // uint32_t d_namlen
    TEST_ASSERT_EQUAL_UINT32(VFS_FILETYPE_REGULAR_FILE,
                             (uint32_t) * (buf + 20)); // vfs_filetype_t d_type
    TEST_ASSERT_EQUAL_STRING_LEN("a", buf + 24, 1);    // filename

    r = TRY_DRV(virt, ReadDir, 0, buf, 40, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(27, bufUsed);
    TEST_ASSERT_EQUAL(2, cookie);

    TEST_ASSERT_EQUAL_UINT64(2, (uint64_t)*buf);          // uint64_t d_next
    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t) * (buf + 8));  // uint64_t d_ino
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t) * (buf + 16)); // uint32_t d_namlen
    TEST_ASSERT_EQUAL_UINT32(VFS_FILETYPE_DIRECTORY,
                             (uint32_t) * (buf + 20)); // vfs_filetype_t d_type
    TEST_ASSERT_EQUAL_STRING_LEN("dir", buf + 24, 3);  // filename
}

TEST(vfs_virtual_readdir, ReadDirInDir) {
    int r;
    uint8_t buf[BUF_LEN];
    uint64_t cookie = 0;
    size_t bufUsed = 0;

    r = TRY_DRV(virt, Open, "dir", 0);
    r = TRY_DRV(virt, ReadDir, r, buf, 40, &cookie, &bufUsed);
    TEST_ASSERT_EQUAL(0, r);
    TEST_ASSERT_EQUAL(25, bufUsed);
    TEST_ASSERT_EQUAL(1, cookie);

    TEST_ASSERT_EQUAL_UINT64(1, (uint64_t)*buf);          // uint64_t d_next
    TEST_ASSERT_EQUAL_UINT64(0, (uint64_t) * (buf + 8));  // uint64_t d_ino
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t) * (buf + 16)); // uint32_t d_namlen
    TEST_ASSERT_EQUAL_UINT32(VFS_FILETYPE_REGULAR_FILE,
                             (uint32_t) * (buf + 20)); // vfs_filetype_t d_type
    TEST_ASSERT_EQUAL_STRING_LEN("b", buf + 24, 1);    // filename
}

TEST_GROUP_RUNNER(vfs_virtual_readdir) {
    RUN_TEST_CASE(vfs_virtual_readdir, ReadDirFail);
    RUN_TEST_CASE(vfs_virtual_readdir, ReadDirOk);
    RUN_TEST_CASE(vfs_virtual_readdir, ReadDirTwice);
    RUN_TEST_CASE(vfs_virtual_readdir, ReadDirInDir);
}
