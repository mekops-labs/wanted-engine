#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <vfs-devfs.h>
#include <vfs-pipe.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* Each test group that needs a VFS with a pipe driver gets its own fixture. */
static vfs_ctx_t vfs;

static void SetupPipeVfs(void) {
    vfs = VfsInit();
    vfs_driver_t *pipe_drv = PipeDriverCreate();
    DevFs_Register(vfs, "pipe", pipe_drv);
}

/***************************************/
TEST_GROUP(pipe_create);
/***************************************/

TEST_SETUP(pipe_create) {}

TEST_TEAR_DOWN(pipe_create) {}

TEST(pipe_create, ReturnsNonNull) {
    vfs_driver_t *drv = PipeDriverCreate();
    TEST_ASSERT_NOT_NULL(drv);
    drv->Destroy(drv);
}

TEST_GROUP_RUNNER(pipe_create) {
    RUN_TEST_CASE(pipe_create, ReturnsNonNull);
}

/***************************************/
TEST_GROUP(pipe_open_close);
/***************************************/

TEST_SETUP(pipe_open_close) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_open_close) { VfsDestroy(&vfs); }

TEST(pipe_open_close, OpenRootDirectorySucceeds) {
    int fd = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(VFS_TYPE_DEV, vfs->fds[fd].type);
    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
}

TEST(pipe_open_close, OpenNamedPipeReadOnly) {
    int fd = VfsOpen(vfs, "/dev/pipe/chan", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
}

TEST(pipe_open_close, OpenNamedPipeWriteOnly) {
    int fd = VfsOpen(vfs, "/dev/pipe/chan", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
}

TEST(pipe_open_close, OpenRdwrRejected) {
    int fd = VfsOpen(vfs, "/dev/pipe/chan", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(-EINVAL, fd);
}

TEST(pipe_open_close, SamePipeOpenedAsBothEnds) {
    int reader = VfsOpen(vfs, "/dev/pipe/dual", VFS_O_RDONLY);
    int writer = VfsOpen(vfs, "/dev/pipe/dual", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(reader >= 0);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_NOT_EQUAL(reader, writer);
    VfsClose(vfs, reader);
    VfsClose(vfs, writer);
}

TEST_GROUP_RUNNER(pipe_open_close) {
    RUN_TEST_CASE(pipe_open_close, OpenRootDirectorySucceeds);
    RUN_TEST_CASE(pipe_open_close, OpenNamedPipeReadOnly);
    RUN_TEST_CASE(pipe_open_close, OpenNamedPipeWriteOnly);
    RUN_TEST_CASE(pipe_open_close, OpenRdwrRejected);
    RUN_TEST_CASE(pipe_open_close, SamePipeOpenedAsBothEnds);
}

/***************************************/
TEST_GROUP(pipe_rw);
/***************************************/

TEST_SETUP(pipe_rw) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_rw) { VfsDestroy(&vfs); }

TEST(pipe_rw, WriteAndReadBasic) {
    int writer = VfsOpen(vfs, "/dev/pipe/basic", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/basic", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    const char msg[] = "hello-pipe";
    int n = VfsWrite(vfs, writer, msg, sizeof(msg) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), n);

    char buf[32] = {0};
    n = VfsRead(vfs, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), n);
    TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, sizeof(msg) - 1);

    VfsClose(vfs, writer);
    VfsClose(vfs, reader);
}

TEST(pipe_rw, ReadWithNoDataAndWriterActiveReturnsEagain) {
    int writer = VfsOpen(vfs, "/dev/pipe/empty", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/empty", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    char buf[8];
    int n = VfsRead(vfs, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, n);

    VfsClose(vfs, writer);
    VfsClose(vfs, reader);
}

TEST(pipe_rw, ReadAfterWriterClosedWithNoDataReturnsEof) {
    int writer = VfsOpen(vfs, "/dev/pipe/eof", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/eof", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    VfsClose(vfs, writer);

    char buf[8];
    int n = VfsRead(vfs, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, n);

    VfsClose(vfs, reader);
}

TEST(pipe_rw, DataPersistsAfterWriterCloses) {
    int writer = VfsOpen(vfs, "/dev/pipe/persist", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/persist", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    VfsWrite(vfs, writer, "data", 4);
    VfsClose(vfs, writer);

    /* Reader can still drain the buffered data after writer is gone. */
    char buf[8] = {0};
    int n = VfsRead(vfs, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_STRING_LEN("data", buf, 4);

    VfsClose(vfs, reader);
}

TEST(pipe_rw, WriteToFullBufferReturnsEagain) {
    /* 4096-byte ring buffer: write 4096 bytes then one more. */
    int writer = VfsOpen(vfs, "/dev/pipe/full", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/full", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    static uint8_t payload[4096];
    memset(payload, 0xAB, sizeof(payload));

    int n = VfsWrite(vfs, writer, payload, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(4096, n);

    /* Buffer is full — any further write returns -EAGAIN. */
    uint8_t extra = 0xFF;
    n = VfsWrite(vfs, writer, &extra, 1);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, n);

    VfsClose(vfs, writer);
    VfsClose(vfs, reader);
}

TEST(pipe_rw, PartialReadLeavesRemainder) {
    int writer = VfsOpen(vfs, "/dev/pipe/partial", VFS_O_WRONLY);
    int reader = VfsOpen(vfs, "/dev/pipe/partial", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    VfsWrite(vfs, writer, "ABCDE", 5);

    char buf[3] = {0};
    int n = VfsRead(vfs, reader, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("ABC", buf, 3);

    char buf2[4] = {0};
    n = VfsRead(vfs, reader, buf2, 4);
    TEST_ASSERT_EQUAL_INT(2, n);
    TEST_ASSERT_EQUAL_STRING_LEN("DE", buf2, 2);

    VfsClose(vfs, writer);
    VfsClose(vfs, reader);
}

TEST_GROUP_RUNNER(pipe_rw) {
    RUN_TEST_CASE(pipe_rw, WriteAndReadBasic);
    RUN_TEST_CASE(pipe_rw, ReadWithNoDataAndWriterActiveReturnsEagain);
    RUN_TEST_CASE(pipe_rw, ReadAfterWriterClosedWithNoDataReturnsEof);
    RUN_TEST_CASE(pipe_rw, DataPersistsAfterWriterCloses);
    RUN_TEST_CASE(pipe_rw, WriteToFullBufferReturnsEagain);
    RUN_TEST_CASE(pipe_rw, PartialReadLeavesRemainder);
}

/***************************************/
TEST_GROUP(pipe_stat);
/***************************************/

TEST_SETUP(pipe_stat) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_stat) { VfsDestroy(&vfs); }

TEST(pipe_stat, RootIsDirectory) {
    int fd = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    VfsClose(vfs, fd);
}

TEST(pipe_stat, NamedPipeIsCharacterDevice) {
    int fd = VfsOpen(vfs, "/dev/pipe/statchan", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, st.filetype);

    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(pipe_stat) {
    RUN_TEST_CASE(pipe_stat, RootIsDirectory);
    RUN_TEST_CASE(pipe_stat, NamedPipeIsCharacterDevice);
}

/***************************************/
TEST_GROUP(pipe_readdir);
/***************************************/

TEST_SETUP(pipe_readdir) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_readdir) { VfsDestroy(&vfs); }

TEST(pipe_readdir, EmptyDirectoryReturnsZeroUsed) {
    int fd = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[128];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_EQUAL_size_t(0, used);

    VfsClose(vfs, fd);
}

TEST(pipe_readdir, OpenedPipeAppearsInListing) {
    /* Opening a named pipe creates it, so readdir should list it. */
    int chan = VfsOpen(vfs, "/dev/pipe/listed", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(chan >= 0);

    int fd = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[128];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "listed", 6));

    VfsClose(vfs, fd);
    VfsClose(vfs, chan);
}

TEST_GROUP_RUNNER(pipe_readdir) {
    RUN_TEST_CASE(pipe_readdir, EmptyDirectoryReturnsZeroUsed);
    RUN_TEST_CASE(pipe_readdir, OpenedPipeAppearsInListing);
}
