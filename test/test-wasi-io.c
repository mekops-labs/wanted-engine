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
#include <wasi/wasi-internal.h>
#include <wasi/wasi_types.h>

/* Positional I/O over a seekable driver, and poll_oneoff over pipes and the
 * dummy platform's virtual clock, where a sleep advances time instantly. */

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

static uint64_t memTruncated;

static int memTruncate(vfs_driver_ctx_t d, int fd, uint64_t size) {
    (void)d;
    (void)fd;
    memTruncated = size;
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
    memDrv.Truncate = memTruncate;
    memTruncated = 0;
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

/***************************************/
TEST_GROUP(wasi_poll);
/***************************************/

TEST_SETUP(wasi_poll) { setupVfs(); }
TEST_TEAR_DOWN(wasi_poll) { teardownVfs(); }

static __wasi_subscription_t clockSub(uint64_t userdata, uint64_t timeout,
                                      uint16_t flags) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = userdata;
    s.type = __WASI_EVENTTYPE_CLOCK;
    s.u.clock.id = 1; /* monotonic */
    s.u.clock.timeout = timeout;
    s.u.clock.flags = flags;
    return s;
}

static __wasi_subscription_t fdSub(uint64_t userdata, uint8_t type, int fd) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = userdata;
    s.type = type;
    s.u.fd_readwrite.fd = (uint32_t)fd;
    return s;
}

TEST(wasi_poll, TheLayoutMatchesPreview1) {
    TEST_ASSERT_EQUAL_UINT(48, sizeof(__wasi_subscription_t));
    TEST_ASSERT_EQUAL_UINT(16, offsetof(__wasi_subscription_t, u));
    TEST_ASSERT_EQUAL_UINT(32, sizeof(__wasi_event_t));
    TEST_ASSERT_EQUAL_UINT(8, offsetof(__wasi_event_t, error));
    TEST_ASSERT_EQUAL_UINT(10, offsetof(__wasi_event_t, type));
    TEST_ASSERT_EQUAL_UINT(16, offsetof(__wasi_event_t, fd_readwrite));
}

TEST(wasi_poll, NoSubscriptionsIsInvalid) {
    __wasi_event_t ev[1];
    uint32_t n = 0;
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_INVAL,
                             WasiPollOneoff(vfs, NULL, ev, 0, &n));
}

TEST(wasi_poll, ARelativeClockFiresAfterItsTimeout) {
    __wasi_subscription_t s = clockSub(7, 5000000, 0);
    __wasi_event_t ev[1];
    uint32_t n = 0;
    plat_timestamp_t before = 0;
    plat_timestamp_t after = 0;

    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &before);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &after);
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT64(7, ev[0].userdata);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_CLOCK, ev[0].type);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS, ev[0].error);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(5000000, after - before);
}

TEST(wasi_poll, AnAbsoluteClockInThePastFiresAtOnce) {
    __wasi_subscription_t s =
        clockSub(1, 0, __WASI_SUBCLOCKFLAGS_SUBSCRIPTION_CLOCK_ABSTIME);
    __wasi_event_t ev[1];
    uint32_t n = 0;
    DummyClockAdvance(1000000000ULL);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
}

TEST(wasi_poll, AnEmptyPipeWaitsForTheClock) {
    int rd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    __wasi_subscription_t s[2] = {fdSub(1, __WASI_EVENTTYPE_FD_READ, rd),
                                  clockSub(2, 3000000, 0)};
    __wasi_event_t ev[2];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, s, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT64(2, ev[0].userdata);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_CLOCK, ev[0].type);
}

TEST(wasi_poll, BufferedDataIsReadableWithItsCount) {
    int rd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    int wr = VfsOpen(vfs, "/dev/pipe/p", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(3, VfsWrite(vfs, wr, "abc", 3));
    __wasi_subscription_t s[2] = {fdSub(1, __WASI_EVENTTYPE_FD_READ, rd),
                                  clockSub(2, 3000000, 0)};
    __wasi_event_t ev[2];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, s, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_FD_READ, ev[0].type);
    TEST_ASSERT_EQUAL_UINT64(3, ev[0].fd_readwrite.nbytes);
    TEST_ASSERT_EQUAL_UINT16(0, ev[0].fd_readwrite.flags);
}

TEST(wasi_poll, AClosedWriterIsAHangup) {
    int rd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    int wr = VfsOpen(vfs, "/dev/pipe/p", VFS_O_WRONLY);
    VfsClose(vfs, wr);
    __wasi_subscription_t s = fdSub(1, __WASI_EVENTTYPE_FD_READ, rd);
    __wasi_event_t ev[1];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT16(__WASI_EVENTRWFLAGS_FD_READWRITE_HANGUP,
                             ev[0].fd_readwrite.flags);
}

TEST(wasi_poll, EveryReadySubscriptionIsReported) {
    int a = VfsOpen(vfs, "/dev/pipe/a", VFS_O_WRONLY);
    int b = VfsOpen(vfs, "/dev/pipe/b", VFS_O_WRONLY);
    __wasi_subscription_t s[3] = {fdSub(1, __WASI_EVENTTYPE_FD_WRITE, a),
                                  clockSub(2, 3000000, 0),
                                  fdSub(3, __WASI_EVENTTYPE_FD_WRITE, b)};
    __wasi_event_t ev[3];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, s, ev, 3, &n));
    TEST_ASSERT_EQUAL_UINT32(2, n);
    TEST_ASSERT_EQUAL_UINT64(1, ev[0].userdata);
    TEST_ASSERT_EQUAL_UINT64(3, ev[1].userdata);
}

TEST(wasi_poll, ABadFdFiresWithItsError) {
    __wasi_subscription_t s = fdSub(9, __WASI_EVENTTYPE_FD_READ, 30);
    __wasi_event_t ev[1];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT64(9, ev[0].userdata);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_BADF, ev[0].error);
}

TEST(wasi_poll, AnUnknownTypeFiresInvalid) {
    __wasi_subscription_t s = fdSub(4, 9, 0);
    __wasi_event_t ev[1];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_INVAL, ev[0].error);
}

TEST(wasi_poll, AnUnknownClockFiresInvalid) {
    __wasi_subscription_t s = clockSub(5, 1000, 0);
    __wasi_event_t ev[1];
    uint32_t n = 0;

    s.u.clock.id = 42;
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_INVAL, ev[0].error);
}

/* VfsDestroy closes the wake descriptor the fixture hands over. */
TEST(wasi_poll, AStopEndsTheWait) {
    int wake = PlatformWakeCreate();
    TEST_ASSERT_GREATER_OR_EQUAL(0, wake);
    VfsSetWakeFd(vfs, wake);
    PlatformWakeRaise(wake);
    int rd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    __wasi_subscription_t s = fdSub(1, __WASI_EVENTTYPE_FD_READ, rd);
    __wasi_event_t ev[1];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_INTR,
                             WasiPollOneoff(vfs, &s, ev, 1, &n));
    TEST_ASSERT_EQUAL_UINT32(0, n);
}

TEST_GROUP_RUNNER(wasi_poll) {
    RUN_TEST_CASE(wasi_poll, TheLayoutMatchesPreview1);
    RUN_TEST_CASE(wasi_poll, NoSubscriptionsIsInvalid);
    RUN_TEST_CASE(wasi_poll, ARelativeClockFiresAfterItsTimeout);
    RUN_TEST_CASE(wasi_poll, AnAbsoluteClockInThePastFiresAtOnce);
    RUN_TEST_CASE(wasi_poll, AnEmptyPipeWaitsForTheClock);
    RUN_TEST_CASE(wasi_poll, BufferedDataIsReadableWithItsCount);
    RUN_TEST_CASE(wasi_poll, AClosedWriterIsAHangup);
    RUN_TEST_CASE(wasi_poll, EveryReadySubscriptionIsReported);
    RUN_TEST_CASE(wasi_poll, ABadFdFiresWithItsError);
    RUN_TEST_CASE(wasi_poll, AnUnknownTypeFiresInvalid);
    RUN_TEST_CASE(wasi_poll, AnUnknownClockFiresInvalid);
    RUN_TEST_CASE(wasi_poll, AStopEndsTheWait);
}

/***************************************/
TEST_GROUP(vfs_fdops);
/***************************************/

TEST_SETUP(vfs_fdops) { setupVfs(); }
TEST_TEAR_DOWN(vfs_fdops) { teardownVfs(); }

TEST(vfs_fdops, TellReportsTheOffset) {
    char buf[4];
    long pos = -1;
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDONLY);
    VfsRead(vfs, fd, buf, 3);
    TEST_ASSERT_EQUAL_INT(0, VfsTell(vfs, fd, &pos));
    TEST_ASSERT_EQUAL_INT32(3, pos);

    int p = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ESPIPE, VfsTell(vfs, p, &pos));
}

TEST(vfs_fdops, SyncSucceedsWhereNothingIsBuffered) {
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, VfsSync(vfs, fd));
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsSync(vfs, 30));
}

TEST(vfs_fdops, TruncateReachesTheDriverOrIsInvalid) {
    int fd = VfsOpen(vfs, "/mem", VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, VfsTruncate(vfs, fd, 8));
    TEST_ASSERT_EQUAL_UINT64(8, memTruncated);

    int p = VfsOpen(vfs, "/dev/pipe/p", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, VfsTruncate(vfs, p, 0));
}

TEST(vfs_fdops, NonblockChangesAtRuntime) {
    char buf[4];
    int fd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(0, VfsFlags(vfs, fd) & VFS_O_NONBLOCK);

    TEST_ASSERT_EQUAL_INT(0, VfsSetFlags(vfs, fd, VFS_O_NONBLOCK));
    TEST_ASSERT_EQUAL_INT(VFS_O_NONBLOCK, VfsFlags(vfs, fd) & VFS_O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(vfs, fd, buf, sizeof(buf)));

    TEST_ASSERT_EQUAL_INT(0, VfsSetFlags(vfs, fd, 0));
    TEST_ASSERT_EQUAL_INT(0, VfsFlags(vfs, fd) & VFS_O_NONBLOCK);
}

TEST(vfs_fdops, SyncFlagsCannotChange) {
    int fd = VfsOpen(vfs, "/dev/pipe/p", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOTSUP, VfsSetFlags(vfs, fd, VFS_O_SYNC));
}

TEST(vfs_fdops, RenumberMovesTheDescriptor) {
    char buf[4] = {0};
    int a = VfsOpen(vfs, "/dev/pipe/x", VFS_O_RDONLY);
    int b = VfsOpen(vfs, "/dev/pipe/y", VFS_O_RDONLY);
    int w = VfsOpen(vfs, "/dev/pipe/x", VFS_O_WRONLY);
    VfsWrite(vfs, w, "hi", 2);

    TEST_ASSERT_EQUAL_INT(0, VfsRenumber(vfs, a, b));
    TEST_ASSERT_EQUAL_INT(2, VfsRead(vfs, b, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY("hi", buf, 2);
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsRead(vfs, a, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-EBADF, VfsRenumber(vfs, a, b));
}

TEST_GROUP_RUNNER(vfs_fdops) {
    RUN_TEST_CASE(vfs_fdops, TellReportsTheOffset);
    RUN_TEST_CASE(vfs_fdops, SyncSucceedsWhereNothingIsBuffered);
    RUN_TEST_CASE(vfs_fdops, TruncateReachesTheDriverOrIsInvalid);
    RUN_TEST_CASE(vfs_fdops, NonblockChangesAtRuntime);
    RUN_TEST_CASE(vfs_fdops, SyncFlagsCannotChange);
    RUN_TEST_CASE(vfs_fdops, RenumberMovesTheDescriptor);
}
