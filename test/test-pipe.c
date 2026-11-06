/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE /* RUSAGE_THREAD */

#include "unity_fixture.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vfs-drivers.h>

#include "test-utils.h"

#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-pipe.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wasi/wasi-internal.h>
#include <wasi/wasi_types.h>

/* Each test group that needs a VFS with a pipe driver gets its own fixture.
 * The pipe storage now lives in a shared pipe_store_t the fixture owns; the
 * driver only references it, so the store is freed separately in teardown. */
static vfs_ctx_t vfs;
static pipe_store_t *store;

static void SetupPipeVfs(void) {
    vfs = VfsInit();
    store = PipeStoreNew();
    vfs_driver_t *pipe_drv = PipeDriverCreate(store);
    DevFs_Register(vfs, "pipe", pipe_drv);
}

static void TeardownPipeVfs(void) {
    VfsDestroy(&vfs);
    PipeStoreFree(store);
    store = NULL;
}

/***************************************/
TEST_GROUP(pipe_create);
/***************************************/

TEST_SETUP(pipe_create) {}

TEST_TEAR_DOWN(pipe_create) {}

TEST(pipe_create, ReturnsNonNull) {
    pipe_store_t *st = PipeStoreNew();
    TEST_ASSERT_NOT_NULL(st);
    vfs_driver_t *drv = PipeDriverCreate(st);
    TEST_ASSERT_NOT_NULL(drv);
    drv->Destroy(drv);
    PipeStoreFree(st);
}

TEST_GROUP_RUNNER(pipe_create) { RUN_TEST_CASE(pipe_create, ReturnsNonNull); }

/***************************************/
TEST_GROUP(pipe_open_close);
/***************************************/

TEST_SETUP(pipe_open_close) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_open_close) { TeardownPipeVfs(); }

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

TEST_TEAR_DOWN(pipe_rw) { TeardownPipeVfs(); }

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
    /* O_NONBLOCK: a blocking read would poll-loop to the safety cap here, since
     * a writer is attached but has produced no data. */
    int reader = VfsOpen(vfs, "/dev/pipe/empty", VFS_O_RDONLY | VFS_O_NONBLOCK);
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

TEST(pipe_rw, NonblockWriteToFullBufferReturnsEagainAtOnce) {
    int writer =
        VfsOpen(vfs, "/dev/pipe/nbfull", VFS_O_WRONLY | VFS_O_NONBLOCK);
    int reader = VfsOpen(vfs, "/dev/pipe/nbfull", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    static uint8_t payload[CONFIG_WANTED_PIPE_BUF_SIZE];
    memset(payload, 0xAB, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(CONFIG_WANTED_PIPE_BUF_SIZE,
                          VfsWrite(vfs, writer, payload, sizeof(payload)));

    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsWrite(vfs, writer, &extra, 1));

    /* Draining makes room, and the same writer then makes progress. */
    uint8_t got[4] = {0};
    TEST_ASSERT_EQUAL_INT((int)sizeof(got),
                          VfsRead(vfs, reader, got, sizeof(got)));
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(vfs, writer, &extra, 1));

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
    RUN_TEST_CASE(pipe_rw, NonblockWriteToFullBufferReturnsEagainAtOnce);
    RUN_TEST_CASE(pipe_rw, PartialReadLeavesRemainder);
}

/***************************************/
TEST_GROUP(pipe_stat);
/***************************************/

TEST_SETUP(pipe_stat) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_stat) { TeardownPipeVfs(); }

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

TEST_TEAR_DOWN(pipe_readdir) { TeardownPipeVfs(); }

TEST(pipe_readdir, EmptyDirectoryReturnsZeroUsed) {
    int fd = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[128];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
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
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "listed", 6));

    VfsClose(vfs, fd);
    VfsClose(vfs, chan);
}

TEST_GROUP_RUNNER(pipe_readdir) {
    RUN_TEST_CASE(pipe_readdir, EmptyDirectoryReturnsZeroUsed);
    RUN_TEST_CASE(pipe_readdir, OpenedPipeAppearsInListing);
}

/***************************************/
TEST_GROUP(pipe_shared);
/***************************************/

/* Two independent driver instances over ONE shared store model two wapps:
 * each wapp has its own /dev/pipe driver + handle table, but the named pipes
 * live in the engine-owned store both reference. */
static pipe_store_t *shared;
static vfs_ctx_t vfs_a;
static vfs_ctx_t vfs_b;

TEST_SETUP(pipe_shared) {
    shared = PipeStoreNew();
    vfs_a = VfsInit();
    vfs_b = VfsInit();
    DevFs_Register(vfs_a, "pipe", PipeDriverCreate(shared));
    DevFs_Register(vfs_b, "pipe", PipeDriverCreate(shared));
}

TEST_TEAR_DOWN(pipe_shared) {
    VfsDestroy(&vfs_a);
    VfsDestroy(&vfs_b);
    PipeStoreFree(shared);
    shared = NULL;
}

TEST(pipe_shared, WriteOnOneDriverReadOnAnother) {
    int writer = VfsOpen(vfs_a, "/dev/pipe/xchan", VFS_O_WRONLY);
    int reader = VfsOpen(vfs_b, "/dev/pipe/xchan", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    const char msg[] = "cross-wapp";
    int n = VfsWrite(vfs_a, writer, msg, sizeof(msg) - 1);
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), n);

    /* Data is already buffered, so this blocking read returns immediately. */
    char buf[32] = {0};
    n = VfsRead(vfs_b, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1), n);
    TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, sizeof(msg) - 1);

    VfsClose(vfs_a, writer);
    VfsClose(vfs_b, reader);
}

TEST(pipe_shared, ReaderSeesEofAfterWriterInOtherWappCloses) {
    int writer = VfsOpen(vfs_a, "/dev/pipe/eofx", VFS_O_WRONLY);
    int reader = VfsOpen(vfs_b, "/dev/pipe/eofx", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);

    /* writer_seen spans wapps: once the only writer (in vfs_a) closes, the
     * reader in vfs_b gets EOF rather than blocking. */
    VfsClose(vfs_a, writer);

    char buf[8];
    int n = VfsRead(vfs_b, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, n);

    VfsClose(vfs_b, reader);
}

TEST(pipe_shared, NonblockReadBeforeAnyWriterReturnsEagain) {
    /* No writer has ever attached → writer_seen is false → not EOF. O_NONBLOCK
     * surfaces the would-block as -EAGAIN instead of polling. */
    int reader =
        VfsOpen(vfs_b, "/dev/pipe/early", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(reader >= 0);

    char buf[8];
    int n = VfsRead(vfs_b, reader, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, n);

    VfsClose(vfs_b, reader);
}

TEST_GROUP_RUNNER(pipe_shared) {
    RUN_TEST_CASE(pipe_shared, WriteOnOneDriverReadOnAnother);
    RUN_TEST_CASE(pipe_shared, ReaderSeesEofAfterWriterInOtherWappCloses);
    RUN_TEST_CASE(pipe_shared, NonblockReadBeforeAnyWriterReturnsEagain);
}

/***************************************/
TEST_GROUP(pipe_console);
/***************************************/

/* A console slot is installed straight into a stdio fd by VfsRegister, so the
 * two ends are separate drivers over one named pipe in the shared store. */
static pipe_store_t *cstore;
static vfs_ctx_t cvfs;

TEST_SETUP(pipe_console) {
    cstore = PipeStoreNew();
    cvfs = VfsInit();
}

TEST_TEAR_DOWN(pipe_console) {
    VfsDestroy(&cvfs);
    PipeStoreFree(cstore);
    cstore = NULL;
}

static void consoleOut(const char *name) {
    VfsRegister(cvfs, "<stdout>",
                VfsPipeConsoleCreate(cstore, name, false, VFS_O_WRONLY,
                                     PIPE_EOF_NONE, NULL));
}

static void consoleOutNonblock(const char *name) {
    VfsRegister(cvfs, "<stdout>",
                VfsPipeConsoleCreate(cstore, name, false,
                                     VFS_O_WRONLY | VFS_O_NONBLOCK,
                                     PIPE_EOF_NONE, NULL));
}

static void consoleInNonblock(const char *name) {
    VfsRegister(cvfs, "<stdin>",
                VfsPipeConsoleCreate(cstore, name, true,
                                     VFS_O_RDONLY | VFS_O_NONBLOCK,
                                     PIPE_EOF_NONE, NULL));
}

static void consoleIn(const char *name) {
    VfsRegister(cvfs, "<stdin>",
                VfsPipeConsoleCreate(cstore, name, true, VFS_O_RDONLY,
                                     PIPE_EOF_NONE, NULL));
}

/* A console writer delivers or refuses; it never discards buffered bytes to
 * make room, which would corrupt a framed stream without telling either end.
 * O_NONBLOCK gives a producer that cannot wait an immediate refusal. */
TEST(pipe_console, NonblockWriterRefusesAFullRingAtOnce) {
    consoleOutNonblock("nbwire");

    static uint8_t payload[CONFIG_WANTED_PIPE_BUF_SIZE];
    memset(payload, 0xAB, sizeof(payload));
    TEST_ASSERT_EQUAL_INT(CONFIG_WANTED_PIPE_BUF_SIZE,
                          VfsWrite(cvfs, VFS_STDOUT, payload, sizeof(payload)));

    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsWrite(cvfs, VFS_STDOUT, &extra, 1));
}

/* The first bytes written must still be the first bytes read: a ring that
 * dropped its oldest to fit would hand the reader a stream missing its head. */
TEST(pipe_console, KeepsTheOldestBytesWhenTheRingFills) {
    consoleOutNonblock("head");
    consoleIn("head");

    static uint8_t payload[CONFIG_WANTED_PIPE_BUF_SIZE];
    memset(payload, 0xAB, sizeof(payload));
    payload[0] = 0x01;
    TEST_ASSERT_EQUAL_INT(CONFIG_WANTED_PIPE_BUF_SIZE,
                          VfsWrite(cvfs, VFS_STDOUT, payload, sizeof(payload)));
    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsWrite(cvfs, VFS_STDOUT, &extra, 1));

    uint8_t got[4] = {0};
    TEST_ASSERT_EQUAL_INT((int)sizeof(got),
                          VfsRead(cvfs, VFS_STDIN, got, sizeof(got)));
    TEST_ASSERT_EQUAL_UINT8(0x01, got[0]);
}

/* A producer restarting detaches its writer for a moment. Its consumer must
 * read that as "nothing yet", or it would exit on a peer that is coming back.
 */
TEST(pipe_console, WriterLeavingIsNotEndOfStream) {
    consoleOut("restart");
    consoleInNonblock("restart");

    VfsClose(cvfs, VFS_STDOUT); /* the producer exits */

    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(cvfs, VFS_STDIN, buf, sizeof(buf)));
}

/* Buffered bytes outlive the producer, so a consumer reading after its peer
 * left still receives what it missed. */
TEST(pipe_console, BufferedBytesSurviveTheWriterLeaving) {
    consoleOut("keep");
    consoleIn("keep");

    const char msg[] = "pending";
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1),
                          VfsWrite(cvfs, VFS_STDOUT, msg, sizeof(msg) - 1));
    VfsClose(cvfs, VFS_STDOUT);

    char buf[16] = {0};
    TEST_ASSERT_EQUAL_INT((int)(sizeof(msg) - 1),
                          VfsRead(cvfs, VFS_STDIN, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN(msg, buf, sizeof(msg) - 1);
}

TEST_GROUP_RUNNER(pipe_console) {
    RUN_TEST_CASE(pipe_console, NonblockWriterRefusesAFullRingAtOnce);
    RUN_TEST_CASE(pipe_console, KeepsTheOldestBytesWhenTheRingFills);
    RUN_TEST_CASE(pipe_console, WriterLeavingIsNotEndOfStream);
    RUN_TEST_CASE(pipe_console, BufferedBytesSurviveTheWriterLeaving);
}

/***************************************/
TEST_GROUP(pipe_wake);
/***************************************/

/* A stop raises the wapp's wake descriptor, and every blocking pipe wait must
 * end on it: a signal that lands between two of its sleeps ends none. */
static pipe_store_t *wstore;
static vfs_ctx_t wvfs;
static int wake;

static vfs_driver_t *withWake(vfs_driver_t *drv) {
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->SetWake);
    drv->SetWake(drv->ctx, wake);
    return drv;
}

TEST_SETUP(pipe_wake) {
    wstore = PipeStoreNew();
    wvfs = VfsInit();
    wake = PlatformWakeCreate();
    TEST_ASSERT_TRUE(wake >= 0);
    VfsSetWakeFd(wvfs, wake);
}

TEST_TEAR_DOWN(pipe_wake) {
    VfsDestroy(&wvfs); /* closes the wake descriptor */
    PipeStoreFree(wstore);
    wstore = NULL;
}

TEST(pipe_wake, ReadWithNoWriterEndsOnTheWake) {
    DevFs_Register(wvfs, "pipe", withWake(PipeDriverCreate(wstore)));
    int reader = VfsOpen(wvfs, "/dev/pipe/void", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(reader >= 0);

    PlatformWakeRaise(wake);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINTR, VfsRead(wvfs, reader, buf, sizeof(buf)));
    VfsClose(wvfs, reader);
}

TEST(pipe_wake, WriteToAFullRingEndsOnTheWake) {
    DevFs_Register(wvfs, "pipe", withWake(PipeDriverCreate(wstore)));
    int writer = VfsOpen(wvfs, "/dev/pipe/full", VFS_O_WRONLY);
    int reader = VfsOpen(wvfs, "/dev/pipe/full", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(writer >= 0);
    TEST_ASSERT_TRUE(reader >= 0);
    static uint8_t payload[CONFIG_WANTED_PIPE_BUF_SIZE];
    TEST_ASSERT_EQUAL_INT(CONFIG_WANTED_PIPE_BUF_SIZE,
                          VfsWrite(wvfs, writer, payload, sizeof(payload)));

    PlatformWakeRaise(wake);
    uint8_t extra = 0xFF;
    TEST_ASSERT_EQUAL_INT(-EINTR, VfsWrite(wvfs, writer, &extra, 1));
    VfsClose(wvfs, writer);
    VfsClose(wvfs, reader);
}

TEST(pipe_wake, ConsoleReadEndsOnTheWake) {
    VfsRegister(wvfs, "<stdin>",
                withWake(VfsPipeConsoleCreate(wstore, "in", true, VFS_O_RDONLY,
                                              PIPE_EOF_NONE, NULL)));

    PlatformWakeRaise(wake);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINTR, VfsRead(wvfs, VFS_STDIN, buf, sizeof(buf)));
}

TEST_GROUP_RUNNER(pipe_wake) {
    RUN_TEST_CASE(pipe_wake, ReadWithNoWriterEndsOnTheWake);
    RUN_TEST_CASE(pipe_wake, WriteToAFullRingEndsOnTheWake);
    RUN_TEST_CASE(pipe_wake, ConsoleReadEndsOnTheWake);
}

/***************************************/
TEST_GROUP(pipe_names);
/***************************************/

TEST_SETUP(pipe_names) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_names) { TeardownPipeVfs(); }

TEST(pipe_names, CheckAcceptsTheCharacterSet) {
    TEST_ASSERT_EQUAL_INT(0, PipeNameCheck("a"));
    TEST_ASSERT_EQUAL_INT(0, PipeNameCheck("Az09._-"));
    TEST_ASSERT_EQUAL_INT(0, PipeNameCheck("t.ssh.out"));
}

TEST(pipe_names, CheckRefusesEmptyAndOtherCharacters) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck(NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck(""));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck("a b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck("a/b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck("a=b"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeNameCheck("caf\xc3\xa9"));
}

TEST(pipe_names, CheckRefusesAPastTheLimit) {
    char name[PIPE_NAME_MAX + 3];
    memset(name, 'a', sizeof(name));
    name[PIPE_NAME_MAX] = '\0';
    TEST_ASSERT_EQUAL_INT(0, PipeNameCheck(name));
    name[PIPE_NAME_MAX] = 'a';
    name[PIPE_NAME_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG, PipeNameCheck(name));
}

TEST(pipe_names, ALongNameIsRefusedAtOpenNotTruncated) {
    char path[96] = "/dev/pipe/";
    size_t base = strlen(path);
    memset(path + base, 'a', 40);
    path[base + 40] = '\0';
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG, VfsOpen(vfs, path, VFS_O_RDONLY));
    /* Two names that differ only past the limit never reach one pipe. */
    memset(path + base, 'a', PIPE_NAME_MAX);
    path[base + PIPE_NAME_MAX] = 'x';
    path[base + PIPE_NAME_MAX + 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG, VfsOpen(vfs, path, VFS_O_WRONLY));
}

TEST(pipe_names, ABadCharacterIsRefusedAtOpen) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsOpen(vfs, "/dev/pipe/a b", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsOpen(vfs, "/dev/pipe/a=b", VFS_O_RDONLY));
}

TEST(pipe_names, ConsoleRefusesABadName) {
    TEST_ASSERT_NULL(
        VfsPipeConsoleCreate(store, "a b", false, 0, PIPE_EOF_NONE, NULL));
    TEST_ASSERT_NULL(
        VfsPipeConsoleCreate(store, "", false, 0, PIPE_EOF_NONE, NULL));
}

TEST(pipe_names, ConsoleOptionsParseNameAndEof) {
    char name[PIPE_NAME_MAX + 2];
    pipe_eof_t eof = PIPE_EOF_ANY;
    TEST_ASSERT_EQUAL_INT(0,
                          PipeConsoleOptions(NULL, name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_INT(PIPE_EOF_NONE, eof);
    TEST_ASSERT_EQUAL_STRING("", name);

    TEST_ASSERT_EQUAL_INT(
        0, PipeConsoleOptions("name=wire", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_STRING("wire", name);
    TEST_ASSERT_EQUAL_INT(PIPE_EOF_NONE, eof);

    TEST_ASSERT_EQUAL_INT(
        0, PipeConsoleOptions("eof,name=w.1", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_STRING("w.1", name);
    TEST_ASSERT_EQUAL_INT(PIPE_EOF_ANY, eof);

    TEST_ASSERT_EQUAL_INT(0,
                          PipeConsoleOptions("eof", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_STRING("", name);
    TEST_ASSERT_EQUAL_INT(PIPE_EOF_ANY, eof);

    TEST_ASSERT_EQUAL_INT(
        0, PipeConsoleOptions("name=j,eof=ok", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_STRING("j", name);
    TEST_ASSERT_EQUAL_INT(PIPE_EOF_OK, eof);

    TEST_ASSERT_EQUAL_INT(
        -EINVAL, PipeConsoleOptions("eof=no", name, sizeof(name), &eof));
}

TEST(pipe_names, ConsoleOptionsRefuseWhatTheyDoNotKnow) {
    char name[PIPE_NAME_MAX + 2];
    pipe_eof_t eof;
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, PipeConsoleOptions("bogus", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, PipeConsoleOptions("name=", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, PipeConsoleOptions("name=a b", name, sizeof(name), &eof));
    TEST_ASSERT_EQUAL_INT(
        -ENAMETOOLONG,
        PipeConsoleOptions("name=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                           name, sizeof(name), &eof));
}

TEST_GROUP_RUNNER(pipe_names) {
    RUN_TEST_CASE(pipe_names, CheckAcceptsTheCharacterSet);
    RUN_TEST_CASE(pipe_names, CheckRefusesEmptyAndOtherCharacters);
    RUN_TEST_CASE(pipe_names, CheckRefusesAPastTheLimit);
    RUN_TEST_CASE(pipe_names, ALongNameIsRefusedAtOpenNotTruncated);
    RUN_TEST_CASE(pipe_names, ABadCharacterIsRefusedAtOpen);
    RUN_TEST_CASE(pipe_names, ConsoleRefusesABadName);
    RUN_TEST_CASE(pipe_names, ConsoleOptionsParseNameAndEof);
    RUN_TEST_CASE(pipe_names, ConsoleOptionsRefuseWhatTheyDoNotKnow);
}

/***************************************/
TEST_GROUP(pipe_scope);
/***************************************/

/* Two wapps over one store: `other` reaches every pipe and creates them, the
 * scoped wapp reaches only the names its grant lists. */
static pipe_store_t *sstore;
static vfs_ctx_t scoped;
static vfs_ctx_t other;

static void scopedWith(const char *grant) {
    DevFs_Register(scoped, "pipe", PipeDriverCreateScoped(sstore, grant));
}

TEST_SETUP(pipe_scope) {
    sstore = PipeStoreNew();
    scoped = VfsInit();
    other = VfsInit();
    DevFs_Register(other, "pipe", PipeDriverCreate(sstore));
}

TEST_TEAR_DOWN(pipe_scope) {
    VfsDestroy(&scoped);
    VfsDestroy(&other);
    PipeStoreFree(sstore);
    sstore = NULL;
}

TEST(pipe_scope, AGrantOpensItsNamesInTheirDirections) {
    scopedWith("read=in,write=out,read=both,write=both");
    int r = VfsOpen(scoped, "/dev/pipe/in", VFS_O_RDONLY);
    int w = VfsOpen(scoped, "/dev/pipe/out", VFS_O_WRONLY);
    int br = VfsOpen(scoped, "/dev/pipe/both", VFS_O_RDONLY);
    int bw = VfsOpen(scoped, "/dev/pipe/both", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_TRUE(w >= 0);
    TEST_ASSERT_TRUE(br >= 0);
    TEST_ASSERT_TRUE(bw >= 0);
    VfsClose(scoped, r);
    VfsClose(scoped, w);
    VfsClose(scoped, br);
    VfsClose(scoped, bw);
}

TEST(pipe_scope, AnUngrantedNameIsAbsentEvenWhenThePipeExists) {
    scopedWith("read=mine");
    int theirs = VfsOpen(other, "/dev/pipe/theirs", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(theirs >= 0);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(scoped, "/dev/pipe/theirs", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(scoped, "/dev/pipe/never", VFS_O_WRONLY));
    VfsClose(other, theirs);
}

TEST(pipe_scope, TheOtherDirectionIsRefused) {
    scopedWith("read=in,write=out");
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          VfsOpen(scoped, "/dev/pipe/in", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          VfsOpen(scoped, "/dev/pipe/out", VFS_O_RDONLY));
}

TEST(pipe_scope, ReadWriteIsRefusedBeforeTheScopeIsConsulted) {
    scopedWith("read=in");
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsOpen(scoped, "/dev/pipe/in", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          VfsOpen(scoped, "/dev/pipe/ghost", VFS_O_RDWR));
}

TEST(pipe_scope, AGrantedPipeCarriesDataToAnotherWapp) {
    scopedWith("write=wire");
    int w = VfsOpen(scoped, "/dev/pipe/wire", VFS_O_WRONLY);
    int r = VfsOpen(other, "/dev/pipe/wire", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(w >= 0);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(scoped, w, "ok", 2));
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(2, VfsRead(other, r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("ok", buf, 2);
    VfsClose(scoped, w);
    VfsClose(other, r);
}

TEST(pipe_scope, TheListingHoldsOnlyGrantedNames) {
    scopedWith("read=mine");
    int mine = VfsOpen(scoped, "/dev/pipe/mine", VFS_O_RDONLY);
    int theirs = VfsOpen(other, "/dev/pipe/theirs", VFS_O_RDONLY);
    int dir = VfsOpen(scoped, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(mine >= 0);
    TEST_ASSERT_TRUE(theirs >= 0);
    TEST_ASSERT_TRUE(dir >= 0);

    uint8_t buf[128];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(scoped, dir, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "mine", 4));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "theirs", 6));

    VfsClose(scoped, dir);
    VfsClose(scoped, mine);
    VfsClose(other, theirs);
}

TEST(pipe_scope, AnEmptyScopeReachesNothingButHasADirectory) {
    scopedWith(NULL);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(scoped, "/dev/pipe/x", VFS_O_RDONLY));
    int dir = VfsOpen(scoped, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(dir >= 0);
    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 1;
    TEST_ASSERT_EQUAL_INT(
        0, VfsReadDir(scoped, dir, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_EQUAL_size_t(0, used);
    VfsClose(scoped, dir);
}

TEST(pipe_scope, AMalformedGrantIsRefused) {
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "bogus"));
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "read="));
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "read=a b"));
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "rw=a"));
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "read=a,"));
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, "read=a,,write=b"));
}

TEST(pipe_scope, AGrantPastItsCapIsRefused) {
    char grant[CONFIG_WANTED_MAX_OPTIONS_SIZE];
    size_t used = 0;
    for (int i = 0; i <= CONFIG_WANTED_PIPE_GRANTS; i++) {
        int n = snprintf(grant + used, sizeof(grant) - used, "%sread=p%d",
                         i ? "," : "", i);
        TEST_ASSERT_TRUE(n > 0 && used + (size_t)n < sizeof(grant));
        used += (size_t)n;
    }
    TEST_ASSERT_NULL(PipeDriverCreateScoped(sstore, grant));
}

TEST(pipe_scope, GrantsAddUpAcrossCalls) {
    vfs_driver_t *drv = PipeDriverCreateScoped(sstore, NULL);
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_INT(0, PipeDriverGrant(drv, "read=a"));
    TEST_ASSERT_EQUAL_INT(0, PipeDriverGrant(drv, "write=a,read=b"));
    DevFs_Register(scoped, "pipe", drv);
    int ar = VfsOpen(scoped, "/dev/pipe/a", VFS_O_RDONLY);
    int aw = VfsOpen(scoped, "/dev/pipe/a", VFS_O_WRONLY);
    int br = VfsOpen(scoped, "/dev/pipe/b", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(ar >= 0 && aw >= 0 && br >= 0);
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          VfsOpen(scoped, "/dev/pipe/b", VFS_O_WRONLY));
    VfsClose(scoped, ar);
    VfsClose(scoped, aw);
    VfsClose(scoped, br);
}

TEST(pipe_scope, AFailedGrantAddsNothing) {
    vfs_driver_t *drv = PipeDriverCreateScoped(sstore, "read=keep");
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeDriverGrant(drv, "read=new,bogus"));
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG,
                          PipeDriverGrant(drv, "read=new,write=aaaaaaaaaaaaaaaa"
                                               "aaaaaaaaaaaaaaaaaaaa"));
    DevFs_Register(scoped, "pipe", drv);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(scoped, "/dev/pipe/new", VFS_O_RDONLY));
    int keep = VfsOpen(scoped, "/dev/pipe/keep", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(keep >= 0);
    VfsClose(scoped, keep);
}

TEST(pipe_scope, GrantsAcrossCallsShareTheCap) {
    vfs_driver_t *drv = PipeDriverCreateScoped(sstore, NULL);
    TEST_ASSERT_NOT_NULL(drv);
    for (int i = 0; i < CONFIG_WANTED_PIPE_GRANTS; i++) {
        char one[32];
        snprintf(one, sizeof(one), "read=p%d", i);
        TEST_ASSERT_EQUAL_INT(0, PipeDriverGrant(drv, one));
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, PipeDriverGrant(drv, "read=one-more"));
    /* A name already listed adds a direction, not an entry. */
    TEST_ASSERT_EQUAL_INT(0, PipeDriverGrant(drv, "write=p0"));
    drv->Destroy(drv);
}

TEST(pipe_scope, AnUnscopedDriverTakesNoGrant) {
    vfs_driver_t *drv = PipeDriverCreate(sstore);
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeDriverGrant(drv, "read=a"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeDriverGrant(NULL, "read=a"));
    drv->Destroy(drv);
}

TEST(pipe_scope, TheGrantInitDemandsAName) {
    TEST_ASSERT_NULL(VfsPipeInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsPipeInit(NULL, ""));
    TEST_ASSERT_NULL(VfsPipeInit(NULL, "bogus"));
    vfs_driver_t *drv = VfsPipeInit(NULL, "read=boot.in");
    TEST_ASSERT_NOT_NULL(drv);
    drv->Destroy(drv);
}

TEST_GROUP_RUNNER(pipe_scope) {
    RUN_TEST_CASE(pipe_scope, AGrantOpensItsNamesInTheirDirections);
    RUN_TEST_CASE(pipe_scope, AnUngrantedNameIsAbsentEvenWhenThePipeExists);
    RUN_TEST_CASE(pipe_scope, TheOtherDirectionIsRefused);
    RUN_TEST_CASE(pipe_scope, ReadWriteIsRefusedBeforeTheScopeIsConsulted);
    RUN_TEST_CASE(pipe_scope, AGrantedPipeCarriesDataToAnotherWapp);
    RUN_TEST_CASE(pipe_scope, TheListingHoldsOnlyGrantedNames);
    RUN_TEST_CASE(pipe_scope, AnEmptyScopeReachesNothingButHasADirectory);
    RUN_TEST_CASE(pipe_scope, AMalformedGrantIsRefused);
    RUN_TEST_CASE(pipe_scope, AGrantPastItsCapIsRefused);
    RUN_TEST_CASE(pipe_scope, GrantsAddUpAcrossCalls);
    RUN_TEST_CASE(pipe_scope, AFailedGrantAddsNothing);
    RUN_TEST_CASE(pipe_scope, GrantsAcrossCallsShareTheCap);
    RUN_TEST_CASE(pipe_scope, AnUnscopedDriverTakesNoGrant);
    RUN_TEST_CASE(pipe_scope, TheGrantInitDemandsAName);
}

/***************************************/
TEST_GROUP(pipe_eof);
/***************************************/

/* A console with eof ends its stream when the wapp ends, which the tests model
 * by destroying the wapp's VFS. A peer reaches the pipe by path. */
static pipe_store_t *estore;
static vfs_ctx_t wapp;
static vfs_ctx_t peer;
static int32_t exitCode; /* how the modelled wapp ended */

TEST_SETUP(pipe_eof) {
    estore = PipeStoreNew();
    exitCode = WAPP_EXIT_CODE_NONE;
    wapp = VfsInit();
    peer = VfsInit();
    DevFs_Register(peer, "pipe", PipeDriverCreate(estore));
}

TEST_TEAR_DOWN(pipe_eof) {
    VfsDestroy(&wapp);
    VfsDestroy(&peer);
    PipeStoreFree(estore);
    estore = NULL;
}

static void wappOut(const char *name, pipe_eof_t eof) {
    VfsRegister(wapp, "<stdout>",
                VfsPipeConsoleCreate(estore, name, false, VFS_O_WRONLY, eof,
                                     &exitCode));
}

static void wappIn(const char *name, pipe_eof_t eof) {
    VfsRegister(
        wapp, "<stdin>",
        VfsPipeConsoleCreate(estore, name, true, VFS_O_RDONLY, eof, &exitCode));
}

TEST(pipe_eof, TheReaderGetsTheBufferedBytesThenEndOfStream) {
    wappOut("job.out", PIPE_EOF_ANY);
    TEST_ASSERT_EQUAL_INT(3, VfsWrite(wapp, VFS_STDOUT, "abc", 3));
    int r = VfsOpen(peer, "/dev/pipe/job.out", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(r >= 0);

    VfsDestroy(&wapp);

    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(3, VfsRead(peer, r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("abc", buf, 3);
    TEST_ASSERT_EQUAL_INT(0, VfsRead(peer, r, buf, sizeof(buf)));
    uint32_t avail = 0;
    int ready = VfsPoll(peer, r, &avail);
    TEST_ASSERT_TRUE((ready & VFS_POLL_IN) != 0);
    TEST_ASSERT_TRUE((ready & VFS_POLL_HUP) != 0);
    VfsClose(peer, r);
}

TEST(pipe_eof, WithoutEofTheStreamDoesNotEnd) {
    wappOut("svc.out", PIPE_EOF_NONE);
    int r = VfsOpen(peer, "/dev/pipe/svc.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(r >= 0);

    VfsDestroy(&wapp);

    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, TheReaderDoesNotSeeEndOfStreamWhileTheWappRuns) {
    wappOut("run.out", PIPE_EOF_ANY);
    int r = VfsOpen(peer, "/dev/pipe/run.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(r >= 0);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, AnInPipeRefusesWritesOnceItsWappEnds) {
    wappIn("job.in", PIPE_EOF_ANY);
    int w = VfsOpen(peer, "/dev/pipe/job.in", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(w >= 0);
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(peer, w, "hi", 2));

    VfsDestroy(&wapp);

    TEST_ASSERT_EQUAL_INT(-EPIPE, VfsWrite(peer, w, "no", 2));
    uint32_t avail = 0;
    int ready = VfsPoll(peer, w, &avail);
    TEST_ASSERT_TRUE((ready & VFS_POLL_HUP) != 0);
    VfsClose(peer, w);
}

TEST(pipe_eof, AnInPipeDropsTheBytesItNeverRead) {
    wappIn("drop.in", PIPE_EOF_ANY);
    int w = VfsOpen(peer, "/dev/pipe/drop.in", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(4, VfsWrite(peer, w, "left", 4));
    VfsDestroy(&wapp);

    /* A new reader finds an empty pipe, not the bytes of the ended wapp. */
    int r = VfsOpen(peer, "/dev/pipe/drop.in", VFS_O_RDONLY | VFS_O_NONBLOCK);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
    VfsClose(peer, w);
}

TEST(pipe_eof, ANewReaderLiftsTheRefusal) {
    wappIn("again.in", PIPE_EOF_ANY);
    int w = VfsOpen(peer, "/dev/pipe/again.in", VFS_O_WRONLY);
    VfsDestroy(&wapp);
    TEST_ASSERT_EQUAL_INT(-EPIPE, VfsWrite(peer, w, "x", 1));

    int r = VfsOpen(peer, "/dev/pipe/again.in", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(peer, w, "x", 1));
    VfsClose(peer, r);
    VfsClose(peer, w);
}

TEST(pipe_eof, ARestartedWappReachesThePeerOnTheSameDescriptor) {
    wappOut("svc2.out", PIPE_EOF_NONE);
    int r = VfsOpen(peer, "/dev/pipe/svc2.out", VFS_O_RDONLY);
    VfsDestroy(&wapp);
    wapp = VfsInit();
    wappOut("svc2.out", PIPE_EOF_NONE);
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(wapp, VFS_STDOUT, "up", 2));
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(2, VfsRead(peer, r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("up", buf, 2);
    VfsClose(peer, r);
}

TEST(pipe_eof, APeerThatClosedFindsAFreshPipeOnTheNextRun) {
    wappOut("next.out", PIPE_EOF_ANY);
    int r = VfsOpen(peer, "/dev/pipe/next.out", VFS_O_RDONLY);
    VfsDestroy(&wapp);
    char buf[4];
    TEST_ASSERT_EQUAL_INT(0, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);

    wapp = VfsInit();
    wappOut("next.out", PIPE_EOF_ANY);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(wapp, VFS_STDOUT, "n", 1));
    r = VfsOpen(peer, "/dev/pipe/next.out", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(1, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, AnOutputWriterIsToldWhenItsInPeerEnded) {
    /* A pipeline: one wapp's out feeds another's in, which then ends. */
    wappOut("x", PIPE_EOF_NONE);
    vfs_ctx_t sink = VfsInit();
    VfsRegister(sink, "<stdin>",
                VfsPipeConsoleCreate(estore, "x", true, VFS_O_RDONLY,
                                     PIPE_EOF_ANY, NULL));
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(wapp, VFS_STDOUT, "a", 1));
    VfsDestroy(&sink);
    TEST_ASSERT_EQUAL_INT(-EPIPE, VfsWrite(wapp, VFS_STDOUT, "b", 1));
}

/* The modelled wapp ends with `code`, then its VFS goes. */
static void wappEnds(int32_t code) {
    exitCode = code;
    VfsDestroy(&wapp);
}

/* A non-blocking read of a fresh peer descriptor: 0 at end of stream. */
static int peerRead(const char *path) {
    int r = VfsOpen(peer, path, VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(r >= 0);
    char buf[8];
    int n = VfsRead(peer, r, buf, sizeof(buf));
    VfsClose(peer, r);
    return n;
}

TEST(pipe_eof, EofOkEndsTheStreamOnACleanExit) {
    wappOut("ok.out", PIPE_EOF_OK);
    int r = VfsOpen(peer, "/dev/pipe/ok.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    wappEnds(0);
    char buf[4];
    TEST_ASSERT_EQUAL_INT(0, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, EofOkKeepsTheStreamAfterAFailingExit) {
    wappOut("fail.out", PIPE_EOF_OK);
    int r = VfsOpen(peer, "/dev/pipe/fail.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    wappEnds(3);
    char buf[4];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, EofOkKeepsTheStreamAfterATrap) {
    wappOut("trap.out", PIPE_EOF_OK);
    int r = VfsOpen(peer, "/dev/pipe/trap.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    wappEnds(WAPP_EXIT_CODE_NONE);
    char buf[4];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, EofOkKeepsAnInPipeOpenAfterAFailingExit) {
    wappIn("fail.in", PIPE_EOF_OK);
    int w = VfsOpen(peer, "/dev/pipe/fail.in", VFS_O_WRONLY);
    wappEnds(1);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(peer, w, "x", 1));
    VfsClose(peer, w);
}

TEST(pipe_eof, EofOkClosesAnInPipeOnACleanExit) {
    wappIn("ok.in", PIPE_EOF_OK);
    int w = VfsOpen(peer, "/dev/pipe/ok.in", VFS_O_WRONLY);
    wappEnds(0);
    TEST_ASSERT_EQUAL_INT(-EPIPE, VfsWrite(peer, w, "x", 1));
    VfsClose(peer, w);
}

TEST(pipe_eof, AWriterWithoutEofClearsAnEndedStream) {
    /* An eof run ends, then the wapp returns without eof (its restart policy
     * changed) and leaves again: the reader must not see an end. */
    int keep = VfsOpen(peer, "/dev/pipe/sw.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    wappOut("sw.out", PIPE_EOF_ANY);
    wappEnds(0);
    TEST_ASSERT_EQUAL_INT(0, peerRead("/dev/pipe/sw.out"));

    wapp = VfsInit();
    wappOut("sw.out", PIPE_EOF_NONE);
    wappEnds(0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, peerRead("/dev/pipe/sw.out"));
    VfsClose(peer, keep);
}

TEST(pipe_eof, AReturningWriterResumesAnEndedStream) {
    wappOut("back.out", PIPE_EOF_ANY);
    int r = VfsOpen(peer, "/dev/pipe/back.out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    wappEnds(0);
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(0, VfsRead(peer, r, buf, sizeof(buf)));

    wapp = VfsInit();
    wappOut("back.out", PIPE_EOF_ANY);
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(wapp, VFS_STDOUT, "up", 2));
    TEST_ASSERT_EQUAL_INT(2, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST(pipe_eof, TwoSlotsOnOnePipeShareOneStreamThatEndsWithTheWapp) {
    wappOut("both", PIPE_EOF_ANY);
    VfsRegister(wapp, "<stderr>",
                VfsPipeConsoleCreate(estore, "both", false, VFS_O_WRONLY,
                                     PIPE_EOF_ANY, &exitCode));
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(wapp, VFS_STDOUT, "a", 1));
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(wapp, VFS_STDERR, "b", 1));
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(wapp, VFS_STDOUT, "c", 1));
    int r = VfsOpen(peer, "/dev/pipe/both", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(r >= 0);

    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(3, VfsRead(peer, r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("abc", buf, 3);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(peer, r, buf, sizeof(buf)));

    VfsDestroy(&wapp);
    TEST_ASSERT_EQUAL_INT(0, VfsRead(peer, r, buf, sizeof(buf)));
    VfsClose(peer, r);
}

TEST_GROUP_RUNNER(pipe_eof) {
    RUN_TEST_CASE(pipe_eof, TwoSlotsOnOnePipeShareOneStreamThatEndsWithTheWapp);
    RUN_TEST_CASE(pipe_eof, TheReaderGetsTheBufferedBytesThenEndOfStream);
    RUN_TEST_CASE(pipe_eof, WithoutEofTheStreamDoesNotEnd);
    RUN_TEST_CASE(pipe_eof, TheReaderDoesNotSeeEndOfStreamWhileTheWappRuns);
    RUN_TEST_CASE(pipe_eof, AnInPipeRefusesWritesOnceItsWappEnds);
    RUN_TEST_CASE(pipe_eof, AnInPipeDropsTheBytesItNeverRead);
    RUN_TEST_CASE(pipe_eof, ANewReaderLiftsTheRefusal);
    RUN_TEST_CASE(pipe_eof, ARestartedWappReachesThePeerOnTheSameDescriptor);
    RUN_TEST_CASE(pipe_eof, APeerThatClosedFindsAFreshPipeOnTheNextRun);
    RUN_TEST_CASE(pipe_eof, AnOutputWriterIsToldWhenItsInPeerEnded);
    RUN_TEST_CASE(pipe_eof, EofOkEndsTheStreamOnACleanExit);
    RUN_TEST_CASE(pipe_eof, EofOkKeepsTheStreamAfterAFailingExit);
    RUN_TEST_CASE(pipe_eof, EofOkKeepsTheStreamAfterATrap);
    RUN_TEST_CASE(pipe_eof, EofOkKeepsAnInPipeOpenAfterAFailingExit);
    RUN_TEST_CASE(pipe_eof, EofOkClosesAnInPipeOnACleanExit);
    RUN_TEST_CASE(pipe_eof, AWriterWithoutEofClearsAnEndedStream);
    RUN_TEST_CASE(pipe_eof, AReturningWriterResumesAnEndedStream);
}

/***************************************/
TEST_GROUP(pipe_handles);
/***************************************/

TEST_SETUP(pipe_handles) { SetupPipeVfs(); }

TEST_TEAR_DOWN(pipe_handles) { TeardownPipeVfs(); }

/* A wapp holds at most 16 open pipe descriptors, the directory included. */
TEST(pipe_handles, TheSeventeenthDescriptorIsRefused) {
    int fds[16];
    for (int i = 0; i < 16; i++) {
        fds[i] = VfsOpen(vfs, "/dev/pipe/many", VFS_O_RDONLY);
        TEST_ASSERT_TRUE(fds[i] >= 0);
    }
    TEST_ASSERT_EQUAL_INT(-EMFILE,
                          VfsOpen(vfs, "/dev/pipe/many", VFS_O_RDONLY));
    VfsClose(vfs, fds[0]);
    int again = VfsOpen(vfs, "/dev/pipe/many", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(again >= 0);
    VfsClose(vfs, again);
    for (int i = 1; i < 16; i++)
        VfsClose(vfs, fds[i]);
}

TEST(pipe_handles, TheDirectoryCountsAgainstTheCap) {
    int fds[16];
    int dir = VfsOpen(vfs, "/dev/pipe", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(dir >= 0);
    for (int i = 0; i < 15; i++) {
        fds[i] = VfsOpen(vfs, "/dev/pipe/many", VFS_O_RDONLY);
        TEST_ASSERT_TRUE(fds[i] >= 0);
    }
    TEST_ASSERT_EQUAL_INT(-EMFILE,
                          VfsOpen(vfs, "/dev/pipe/many", VFS_O_RDONLY));
    for (int i = 0; i < 15; i++)
        VfsClose(vfs, fds[i]);
    VfsClose(vfs, dir);
}

TEST(pipe_handles, SetFlagsSwitchesAnOpenEndToNonBlocking) {
    int w = VfsOpen(vfs, "/dev/pipe/flip", VFS_O_WRONLY);
    int r = VfsOpen(vfs, "/dev/pipe/flip", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(r >= 0);
    TEST_ASSERT_EQUAL_INT(0, VfsSetFlags(vfs, r, VFS_O_NONBLOCK));
    char buf[4];
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(vfs, r, buf, sizeof(buf)));
    VfsClose(vfs, r);
    VfsClose(vfs, w);
}

TEST(pipe_handles, EndingAWappClosesWhatItLeftOpen) {
    pipe_store_t *shared2 = PipeStoreNew();
    vfs_ctx_t a = VfsInit();
    vfs_ctx_t b = VfsInit();
    DevFs_Register(a, "pipe", PipeDriverCreate(shared2));
    DevFs_Register(b, "pipe", PipeDriverCreate(shared2));
    int w = VfsOpen(a, "/dev/pipe/left", VFS_O_WRONLY);
    int r = VfsOpen(b, "/dev/pipe/left", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(w >= 0);

    VfsDestroy(&a);

    char buf[4];
    TEST_ASSERT_EQUAL_INT(0, VfsRead(b, r, buf, sizeof(buf)));
    VfsClose(b, r);
    VfsDestroy(&b);
    PipeStoreFree(shared2);
}

TEST_GROUP_RUNNER(pipe_handles) {
    RUN_TEST_CASE(pipe_handles, TheSeventeenthDescriptorIsRefused);
    RUN_TEST_CASE(pipe_handles, TheDirectoryCountsAgainstTheCap);
    RUN_TEST_CASE(pipe_handles, SetFlagsSwitchesAnOpenEndToNonBlocking);
    RUN_TEST_CASE(pipe_handles, EndingAWappClosesWhatItLeftOpen);
}

/***************************************/
TEST_GROUP(pipe_runtime);
/***************************************/

/* The supervisor grants and revokes names of a running wapp. `rwapp` holds a
 * launch grant and is bound as "w1"; `rother` reaches every pipe. */
static pipe_store_t *rstore;
static vfs_ctx_t rwapp;
static vfs_ctx_t rother;

static void boundWith(const char *grant) {
    vfs_driver_t *drv = PipeDriverCreateScoped(rstore, grant);
    DevFs_Register(rwapp, "pipe", drv);
    TEST_ASSERT_EQUAL_INT(0, PipeDriverBind(drv, "w1"));
}

static bool listed(vfs_ctx_t v, const char *name) {
    int dir = VfsOpen(v, "/dev/pipe", VFS_O_RDONLY);
    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    bool found;

    TEST_ASSERT_TRUE(dir >= 0);
    TEST_ASSERT_EQUAL_INT(0,
                          VfsReadDir(v, dir, buf, sizeof(buf), &cookie, &used));
    found = HasBytes(buf, used, name, strlen(name));
    VfsClose(v, dir);
    return found;
}

TEST_SETUP(pipe_runtime) {
    rstore = PipeStoreNew();
    rwapp = VfsInit();
    rother = VfsInit();
    DevFs_Register(rother, "pipe", PipeDriverCreate(rstore));
}

TEST_TEAR_DOWN(pipe_runtime) {
    VfsDestroy(&rwapp);
    VfsDestroy(&rother);
    PipeStoreFree(rstore);
    rstore = NULL;
}

TEST(pipe_runtime, AGrantOpensANameAtTheNextOpen) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(rwapp, "/dev/pipe/extra", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=extra"));
    int fd = VfsOpen(rwapp, "/dev/pipe/extra", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(rwapp, fd);
}

TEST(pipe_runtime, AGrantHoldsOneDirection) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "write=out"));
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          VfsOpen(rwapp, "/dev/pipe/out", VFS_O_RDONLY));
    int fd = VfsOpen(rwapp, "/dev/pipe/out", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(rwapp, fd);
}

TEST(pipe_runtime, ARevokeStopsNewOpensAndKeepsOpenDescriptors) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "write=out"));
    int w = VfsOpen(rwapp, "/dev/pipe/out", VFS_O_WRONLY);
    int r = VfsOpen(rother, "/dev/pipe/out", VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_TRUE(w >= 0);
    TEST_ASSERT_TRUE(r >= 0);

    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", true, "write=out"));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(rwapp, "/dev/pipe/out", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(rwapp, w, "ok", 2));
    char buf[4] = {0};
    TEST_ASSERT_EQUAL_INT(2, VfsRead(rother, r, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("ok", buf, 2);
    VfsClose(rwapp, w);
    VfsClose(rother, r);
}

TEST(pipe_runtime, TheListingFollowsTheRuntimeGrants) {
    boundWith("read=base");
    int peerFd = VfsOpen(rother, "/dev/pipe/extra", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(peerFd >= 0);
    TEST_ASSERT_FALSE(listed(rwapp, "extra"));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=extra"));
    TEST_ASSERT_TRUE(listed(rwapp, "extra"));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", true, "read=extra"));
    TEST_ASSERT_FALSE(listed(rwapp, "extra"));
    VfsClose(rother, peerFd);
}

TEST(pipe_runtime, TheListNamesTheRuntimeClauses) {
    boundWith("read=base");
    char buf[128];
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=a,write=b"));
    int n = PipeRuntimeList("w1", buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(15, n);
    TEST_ASSERT_EQUAL_STRING("read=a\nwrite=b\n", buf);
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", true, "read=a"));
    TEST_ASSERT_EQUAL_INT(8, PipeRuntimeList("w1", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("write=b\n", buf);
}

TEST(pipe_runtime, AGrantAppliesWholeOrNotAtAll) {
    boundWith("read=base");
    char buf[128];
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          PipeRuntimeGrant("w1", false, "read=a,write=b c"));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));

    char many[CONFIG_WANTED_MAX_OPTIONS_SIZE];
    size_t used = 0;
    for (int i = 0; i <= CONFIG_WANTED_PIPE_RUNTIME_GRANTS; i++) {
        int n = snprintf(many + used, sizeof(many) - used, "%sread=r%d",
                         i ? "," : "", i);
        TEST_ASSERT_TRUE(n > 0);
        used += (size_t)n;
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, PipeRuntimeGrant("w1", false, many));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
}

TEST(pipe_runtime, ARevokeAppliesWholeOrNotAtAll) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=a"));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          PipeRuntimeGrant("w1", true, "read=a,read=never"));
    char buf[64];
    TEST_ASSERT_EQUAL_INT(7, PipeRuntimeList("w1", buf, sizeof(buf)));
}

TEST(pipe_runtime, ALaunchClauseChangesNothingAndCannotBeRevoked) {
    boundWith("read=base");
    char buf[64];
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=base"));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EPERM, PipeRuntimeGrant("w1", true, "read=base"));
    int fd = VfsOpen(rwapp, "/dev/pipe/base", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(rwapp, fd);
}

TEST(pipe_runtime, AMalformedClauseIsRefused) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeRuntimeGrant("w1", false, "bogus"));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeRuntimeGrant("w1", false, "read="));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeRuntimeGrant("w1", false, ""));
    TEST_ASSERT_EQUAL_INT(-EINVAL, PipeRuntimeGrant("w1", false, NULL));
    TEST_ASSERT_EQUAL_INT(
        -ENAMETOOLONG,
        PipeRuntimeGrant("w1", false, "read=0123456789012345678901234567890x"));
}

TEST(pipe_runtime, AWappThatIsNotRunningHoldsNoGrants) {
    char buf[16];
    TEST_ASSERT_EQUAL_INT(-ENOENT, PipeRuntimeGrant("ghost", false, "read=a"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, PipeRuntimeList("ghost", buf, sizeof(buf)));
}

TEST(pipe_runtime, RuntimeGrantsEndWithTheWapp) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=extra"));
    VfsDestroy(&rwapp);

    char buf[16];
    TEST_ASSERT_EQUAL_INT(-ENOENT, PipeRuntimeGrant("w1", false, "read=a"));
    rwapp = VfsInit();
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(rwapp, "/dev/pipe/extra", VFS_O_RDONLY));
}

TEST(pipe_runtime, ANewLaunchReplacesTheBindingOfItsName) {
    boundWith("read=base");
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=first"));
    vfs_ctx_t again = VfsInit();
    vfs_driver_t *drv = PipeDriverCreateScoped(rstore, "read=base");
    DevFs_Register(again, "pipe", drv);
    TEST_ASSERT_EQUAL_INT(0, PipeDriverBind(drv, "w1"));
    char buf[32];
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
    VfsDestroy(&again);
}

TEST(pipe_runtime, AnAmbientWappTakesAGrantAndChangesNothing) {
    vfs_driver_t *drv = PipeDriverCreate(rstore);
    DevFs_Register(rwapp, "pipe", drv);
    TEST_ASSERT_EQUAL_INT(0, PipeDriverBind(drv, "w1"));
    char buf[16];
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeGrant("w1", false, "read=a"));
    TEST_ASSERT_EQUAL_INT(0, PipeRuntimeList("w1", buf, sizeof(buf)));
    int fd = VfsOpen(rwapp, "/dev/pipe/anything", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(rwapp, fd);
}

TEST_GROUP_RUNNER(pipe_runtime) {
    RUN_TEST_CASE(pipe_runtime, AGrantOpensANameAtTheNextOpen);
    RUN_TEST_CASE(pipe_runtime, AGrantHoldsOneDirection);
    RUN_TEST_CASE(pipe_runtime, ARevokeStopsNewOpensAndKeepsOpenDescriptors);
    RUN_TEST_CASE(pipe_runtime, TheListingFollowsTheRuntimeGrants);
    RUN_TEST_CASE(pipe_runtime, TheListNamesTheRuntimeClauses);
    RUN_TEST_CASE(pipe_runtime, AGrantAppliesWholeOrNotAtAll);
    RUN_TEST_CASE(pipe_runtime, ARevokeAppliesWholeOrNotAtAll);
    RUN_TEST_CASE(pipe_runtime, ALaunchClauseChangesNothingAndCannotBeRevoked);
    RUN_TEST_CASE(pipe_runtime, AMalformedClauseIsRefused);
    RUN_TEST_CASE(pipe_runtime, AWappThatIsNotRunningHoldsNoGrants);
    RUN_TEST_CASE(pipe_runtime, RuntimeGrantsEndWithTheWapp);
    RUN_TEST_CASE(pipe_runtime, ANewLaunchReplacesTheBindingOfItsName);
    RUN_TEST_CASE(pipe_runtime, AnAmbientWappTakesAGrantAndChangesNothing);
}

/***************************************/
TEST_GROUP(pipe_ready);
/***************************************/

/* A wapp blocked on a pipe sleeps on its ready event. Each case runs the
 * blocked call on its own thread against a context with an event, and acts on
 * the pipe from a second context. */

#define READY_SETTLE_US 150000

static pipe_store_t *rstore;
static vfs_ctx_t rvfs; /* the blocked wapp */
static vfs_ctx_t pvfs; /* its peer */
static plat_ready_t *rready;

typedef struct {
    int fd;
    char buf[32];
    int result;
    long switches; /* voluntary context switches the call cost */
    uint64_t pollKind;
    pthread_t thread;
} blocked_t;

static long voluntarySwitches(void) {
    struct rusage ru;

    getrusage(RUSAGE_THREAD, &ru);
    return ru.ru_nvcsw;
}

static void *blockedRead(void *arg) {
    blocked_t *b = arg;
    long before = voluntarySwitches();

    b->result = VfsRead(rvfs, b->fd, b->buf, sizeof(b->buf));
    b->switches = voluntarySwitches() - before;
    return NULL;
}

static void *blockedWrite(void *arg) {
    blocked_t *b = arg;
    static uint8_t more[4] = {1, 2, 3, 4};

    b->result = VfsWrite(rvfs, b->fd, more, sizeof(more));
    return NULL;
}

static void *blockedPoll(void *arg) {
    blocked_t *b = arg;
    __wasi_subscription_t sub[2];
    __wasi_event_t ev[2];
    uint32_t n = 0;
    long before = voluntarySwitches();

    memset(sub, 0, sizeof(sub));
    sub[0].userdata = 1;
    sub[0].type = __WASI_EVENTTYPE_FD_READ;
    sub[0].u.fd_readwrite.fd = (uint32_t)b->fd;
    sub[1].userdata = 2;
    sub[1].type = __WASI_EVENTTYPE_CLOCK;
    sub[1].u.clock.id = PLAT_CLOCKID_MONOTONIC;
    sub[1].u.clock.timeout = 60ULL * 1000000000ULL;
    b->result = (int)WasiPollOneoff(rvfs, sub, ev, 2, &n);
    b->switches = voluntarySwitches() - before;
    b->pollKind = n > 0 ? ev[0].userdata : 0;
    return NULL;
}

static void startBlocked(blocked_t *b, void *(*fn)(void *)) {
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&b->thread, NULL, fn, b));
    usleep(READY_SETTLE_US);
}

static void joinBlocked(blocked_t *b) {
    TEST_ASSERT_EQUAL_INT(0, pthread_join(b->thread, NULL));
}

static vfs_driver_t *withReady(vfs_driver_t *drv, vfs_ctx_t vfs) {
    TEST_ASSERT_NOT_NULL(drv);
    TEST_ASSERT_NOT_NULL(drv->SetWake);
    TEST_ASSERT_NOT_NULL(drv->SetReady);
    drv->SetWake(drv->ctx, VfsWakeFd(vfs));
    drv->SetReady(drv->ctx, VfsReady(vfs));
    return drv;
}

TEST_SETUP(pipe_ready) {
    rstore = PipeStoreNew();
    rvfs = VfsInit();
    pvfs = VfsInit();
    rready = PlatformReadyNew();
    TEST_ASSERT_NOT_NULL(rready);
    VfsSetWakeFd(rvfs, PlatformWakeCreate());
    VfsSetReady(rvfs, rready);
    DevFs_Register(rvfs, "pipe", withReady(PipeDriverCreate(rstore), rvfs));
    DevFs_Register(pvfs, "pipe", PipeDriverCreate(rstore));
}

TEST_TEAR_DOWN(pipe_ready) {
    VfsDestroy(&pvfs);
    VfsDestroy(&rvfs); /* frees the ready event */
    PipeStoreFree(rstore);
    rstore = NULL;
}

TEST(pipe_ready, AnIdleReadWakesOnlyWhenDataArrives) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/idle", VFS_O_RDONLY);
    int wr = VfsOpen(pvfs, "/dev/pipe/idle", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(b.fd >= 0);
    TEST_ASSERT_TRUE(wr >= 0);

    startBlocked(&b, blockedRead);
    usleep(300000);
    TEST_ASSERT_EQUAL_INT(2, VfsWrite(pvfs, wr, "hi", 2));
    joinBlocked(&b);

    TEST_ASSERT_EQUAL_INT(2, b.result);
    TEST_ASSERT_EQUAL_STRING_LEN("hi", b.buf, 2);
    /* A 1 ms recheck costs about 450 switches over this wait. */
    TEST_ASSERT_LESS_THAN_INT(20, (int)b.switches);
}

TEST(pipe_ready, ReadingWakesAWriterBlockedOnAFullRing) {
    blocked_t b = {0};
    static uint8_t fill[CONFIG_WANTED_PIPE_BUF_SIZE];
    b.fd = VfsOpen(rvfs, "/dev/pipe/room", VFS_O_WRONLY);
    int rd = VfsOpen(pvfs, "/dev/pipe/room", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(CONFIG_WANTED_PIPE_BUF_SIZE,
                          VfsWrite(rvfs, b.fd, fill, sizeof(fill)));

    startBlocked(&b, blockedWrite);
    uint8_t got[4];
    TEST_ASSERT_EQUAL_INT(4, VfsRead(pvfs, rd, got, sizeof(got)));
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(4, b.result);
}

TEST(pipe_ready, TheLastWriterClosingWakesAReader) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/last", VFS_O_RDONLY);
    int wr = VfsOpen(pvfs, "/dev/pipe/last", VFS_O_WRONLY);

    startBlocked(&b, blockedRead);
    VfsClose(pvfs, wr);
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(0, b.result);
}

TEST(pipe_ready, TheEndOfAConsoleStreamWakesItsReader) {
    blocked_t b = {0};
    VfsRegister(
        rvfs, "<stdin>",
        withReady(VfsPipeConsoleCreate(rstore, "stream", true, VFS_O_RDONLY,
                                       PIPE_EOF_NONE, NULL),
                  rvfs));
    VfsRegister(pvfs, "<stdout>",
                VfsPipeConsoleCreate(rstore, "stream", false, VFS_O_WRONLY,
                                     PIPE_EOF_ANY, NULL));
    b.fd = VFS_STDIN;

    startBlocked(&b, blockedRead);
    VfsDestroy(&pvfs); /* the writer's wapp ends */
    pvfs = VfsInit();
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(0, b.result);
}

TEST(pipe_ready, AStopEndsAPipeWaitWithEintr) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/stop", VFS_O_RDONLY);

    startBlocked(&b, blockedRead);
    PlatformReadyStop(rready);
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(-EINTR, b.result);
}

TEST(pipe_ready, AWakeWithNoChangeLeavesTheCallRunning) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/spur", VFS_O_RDONLY);
    int wr = VfsOpen(pvfs, "/dev/pipe/spur", VFS_O_WRONLY);

    startBlocked(&b, blockedRead);
    PlatformReadyRaise(rready);
    usleep(READY_SETTLE_US);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(pvfs, wr, "x", 1));
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(1, b.result);
}

TEST(pipe_ready, PollOneoffOnAPipeSleepsUntilDataArrives) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/poll", VFS_O_RDONLY);
    int wr = VfsOpen(pvfs, "/dev/pipe/poll", VFS_O_WRONLY);

    startBlocked(&b, blockedPoll);
    usleep(300000);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(pvfs, wr, "x", 1));
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(__WASI_ERRNO_SUCCESS, b.result);
    TEST_ASSERT_EQUAL_UINT64(1, b.pollKind);
    TEST_ASSERT_LESS_THAN_INT(20, (int)b.switches);
}

TEST(pipe_ready, PollOneoffEndsOnAStop) {
    blocked_t b = {0};
    b.fd = VfsOpen(rvfs, "/dev/pipe/pstop", VFS_O_RDONLY);

    startBlocked(&b, blockedPoll);
    PlatformReadyStop(rready);
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(__WASI_ERRNO_INTR, b.result);
}

TEST(pipe_ready, PollOneoffWatchesAPipeOpenedThroughAConsole) {
    blocked_t b = {0};
    VfsRegister(
        rvfs, "<stdin>",
        withReady(VfsPipeConsoleCreate(rstore, "cpoll", true, VFS_O_RDONLY,
                                       PIPE_EOF_NONE, NULL),
                  rvfs));
    VfsRegister(pvfs, "<stdout>",
                VfsPipeConsoleCreate(rstore, "cpoll", false, VFS_O_WRONLY,
                                     PIPE_EOF_NONE, NULL));
    b.fd = VFS_STDIN;

    startBlocked(&b, blockedPoll);
    TEST_ASSERT_EQUAL_INT(1, VfsWrite(pvfs, VFS_STDOUT, "x", 1));
    joinBlocked(&b);
    TEST_ASSERT_EQUAL_INT(__WASI_ERRNO_SUCCESS, b.result);
    TEST_ASSERT_EQUAL_UINT64(1, b.pollKind);
}

TEST_GROUP_RUNNER(pipe_ready) {
    RUN_TEST_CASE(pipe_ready, AnIdleReadWakesOnlyWhenDataArrives);
    RUN_TEST_CASE(pipe_ready, ReadingWakesAWriterBlockedOnAFullRing);
    RUN_TEST_CASE(pipe_ready, TheLastWriterClosingWakesAReader);
    RUN_TEST_CASE(pipe_ready, TheEndOfAConsoleStreamWakesItsReader);
    RUN_TEST_CASE(pipe_ready, AStopEndsAPipeWaitWithEintr);
    RUN_TEST_CASE(pipe_ready, AWakeWithNoChangeLeavesTheCallRunning);
    RUN_TEST_CASE(pipe_ready, PollOneoffOnAPipeSleepsUntilDataArrives);
    RUN_TEST_CASE(pipe_ready, PollOneoffEndsOnAStop);
    RUN_TEST_CASE(pipe_ready, PollOneoffWatchesAPipeOpenedThroughAConsole);
}
