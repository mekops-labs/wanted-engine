/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <dummy-fs.h>
#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-pipe.h>
#include <vfs.h>

/* Positional I/O over a seekable driver. */

static vfs_ctx_t vfs;
static pipe_store_t *store;

/* ── A 16-byte seekable file mounted at /mem ─────────────────────────────── */

#define MEM_SIZE 16

static uint8_t mem[MEM_SIZE];
static long memPos;

static int memOpen(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)d;
    (void)path;
    (void)flags;
    memPos = 0;
    return 0;
}

static int memClose(vfs_driver_ctx_t d, int fd) {
    (void)d;
    (void)fd;
    return 0;
}

static int memRead(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    size_t left = (memPos < MEM_SIZE) ? (size_t)(MEM_SIZE - memPos) : 0;
    size_t n = nbyte < left ? nbyte : left;
    memcpy(buf, mem + memPos, n);
    memPos += (long)n;
    return (int)n;
}

static int memWrite(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    size_t left = (memPos < MEM_SIZE) ? (size_t)(MEM_SIZE - memPos) : 0;
    size_t n = nbyte < left ? nbyte : left;
    memcpy(mem + memPos, buf, n);
    memPos += (long)n;
    return (int)n;
}

static int memSeek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                   long *pos) {
    (void)d;
    (void)fd;
    long base = (whence == VFS_SEEK_CUR)   ? memPos
                : (whence == VFS_SEEK_END) ? MEM_SIZE
                                           : 0;
    if (base + off < 0)
        return -EINVAL;
    memPos = base + off;
    *pos = memPos;
    return 0;
}

static vfs_driver_t memDrv;

static void setupVfs(void) {
    DummyClockReset();
    vfs = VfsInit();
    store = PipeStoreNew();
    DevFs_Register(vfs, "pipe", PipeDriverCreate(store));

    memset(&memDrv, 0, sizeof(memDrv));
    memDrv.filetype = VFS_FILETYPE_REGULAR_FILE;
    memDrv.Open = memOpen;
    memDrv.Close = memClose;
    memDrv.Read = memRead;
    memDrv.Write = memWrite;
    memDrv.Seek = memSeek;
    VfsMountDriver(vfs, "/mem", &memDrv);
    memcpy(mem, "0123456789abcdef", MEM_SIZE);
}

static void teardownVfs(void) {
    VfsDestroy(&vfs);
    PipeStoreFree(store);
    store = NULL;
}

/***************************************/
TEST_GROUP(wasi_pread);
/***************************************/

TEST_SETUP(wasi_pread) { setupVfs(); }
TEST_TEAR_DOWN(wasi_pread) { teardownVfs(); }

TEST(wasi_pread, ReadsAtOffsetAndKeepsThePosition) {
    char buf[4] = {0};
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDWR);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);

    TEST_ASSERT_EQUAL_INT(2, VfsRead(vfs, fd, buf, 2));
    TEST_ASSERT_EQUAL_INT(3, VfsPread(vfs, fd, buf, 3, 5));
    TEST_ASSERT_EQUAL_MEMORY("567", buf, 3);
    TEST_ASSERT_EQUAL_INT(2, VfsRead(vfs, fd, buf, 2));
    TEST_ASSERT_EQUAL_MEMORY("23", buf, 2);
}

TEST(wasi_pread, WritesAtOffsetAndKeepsThePosition) {
    char buf[4] = {0};
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDWR);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);

    TEST_ASSERT_EQUAL_INT(1, VfsRead(vfs, fd, buf, 1));
    TEST_ASSERT_EQUAL_INT(2, VfsPwrite(vfs, fd, "XY", 2, 10));
    TEST_ASSERT_EQUAL_MEMORY("0123456789XYcdef", mem, MEM_SIZE);
    TEST_ASSERT_EQUAL_INT(1, VfsRead(vfs, fd, buf, 1));
    TEST_ASSERT_EQUAL_MEMORY("1", buf, 1);
}

TEST(wasi_pread, ShortAtTheEnd) {
    char buf[8];
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(2, VfsPread(vfs, fd, buf, sizeof(buf), 14));
    TEST_ASSERT_EQUAL_INT(0, VfsPread(vfs, fd, buf, sizeof(buf), 16));
}

TEST(wasi_pread, APipeCannotSeek) {
    char buf[4];
    int fd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    TEST_ASSERT_EQUAL_INT(-ESPIPE, VfsPread(vfs, fd, buf, 1, 0));
    TEST_ASSERT_EQUAL_INT(-ESPIPE, VfsPwrite(vfs, fd, "x", 1, 0));
}

TEST(wasi_pread, AnOffsetPastTheHostRangeOverflows) {
    char buf[4];
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EOVERFLOW, VfsPread(vfs, fd, buf, 1, UINT64_MAX));
}

TEST(wasi_pread, ABadFdIsRefused) {
    char buf[4];
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsPread(vfs, 30, buf, 1, 0));
}

TEST_GROUP_RUNNER(wasi_pread) {
    RUN_TEST_CASE(wasi_pread, ReadsAtOffsetAndKeepsThePosition);
    RUN_TEST_CASE(wasi_pread, WritesAtOffsetAndKeepsThePosition);
    RUN_TEST_CASE(wasi_pread, ShortAtTheEnd);
    RUN_TEST_CASE(wasi_pread, APipeCannotSeek);
    RUN_TEST_CASE(wasi_pread, AnOffsetPastTheHostRangeOverflows);
    RUN_TEST_CASE(wasi_pread, ABadFdIsRefused);
}
