/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-fb.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>
#include <wasi/wasi-internal.h>
#include <wasi/wasi_types.h>

/* /dev/fb: the per-screen subtree, the grant grammar, and the write, flush and
 * observe contract. Screens are headless in-memory buffers registered by the
 * test, so no panel is involved. */

#define MAIN_W 8
#define MAIN_H 4
#define MAIN_BYTES (MAIN_W * MAIN_H * 2)

static vfs_ctx_t vfs;
static vfs_driver_t *drv;

static void registerScreens(void) {
    fb_screen_desc_t main = {"main",           MAIN_W, MAIN_H,
                             FB_FORMAT_RGB565, NULL,   NULL};
    fb_screen_desc_t aux = {"aux", 4, 2, FB_FORMAT_RGB888, NULL, NULL};
    TEST_ASSERT_EQUAL_INT(0, FbScreenRegister(&main));
    TEST_ASSERT_EQUAL_INT(0, FbScreenRegister(&aux));
}

/* Install a driver with `options` and mount it. Returns false when the grant
 * was rejected, which is a failed launch. */
static bool setupGrant(const char *options) {
    vfs = VfsInit();
    drv = VfsFbInit(NULL, options);
    if (drv == NULL)
        return false;
    DevFs_Register(vfs, "fb", drv);
    return true;
}

static void teardown(void) {
    VfsDestroy(&vfs);
    drv = NULL;
    FbScreensReset();
}

/* Read one node into `buf`, NUL-terminated. Returns the byte count or -errno.
 */
static int readNode(const char *path, char *buf, size_t bufLen) {
    int fd = VfsOpen(vfs, path, VFS_O_RDONLY);
    if (fd < 0)
        return fd;
    int n = VfsRead(vfs, fd, buf, bufLen - 1);
    VfsClose(vfs, fd);
    if (n >= 0)
        buf[n] = '\0';
    return n;
}

static int writeNode(const char *path, const char *payload) {
    int fd = VfsOpen(vfs, path, VFS_O_WRONLY);
    if (fd < 0)
        return fd;
    int n = VfsWrite(vfs, fd, payload, strlen(payload));
    VfsClose(vfs, fd);
    return n;
}

/* The names in a directory, joined by ',' into `out`. Returns 0 or -errno. */
static int listDir(const char *path, char *out, size_t outLen) {
    int fd = VfsOpen(vfs, path, VFS_O_RDONLY);
    if (fd < 0)
        return fd;

    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int rc = VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used);
    VfsClose(vfs, fd);
    if (rc < 0)
        return rc;

    out[0] = '\0';
    size_t off = 0;
    while (off + sizeof(vfs_dirent_t) <= used) {
        vfs_dirent_t ent;
        memcpy(&ent, buf + off, sizeof(ent));
        off += sizeof(ent);
        if (out[0] != '\0')
            strncat(out, ",", outLen - strlen(out) - 1);
        strncat(out, (const char *)buf + off,
                (ent.d_namlen < outLen - strlen(out) - 1)
                    ? ent.d_namlen
                    : outLen - strlen(out) - 1);
        off += ent.d_namlen;
    }
    return 0;
}

/***************************************/
TEST_GROUP(fb_grant);
/***************************************/

TEST_SETUP(fb_grant) { registerScreens(); }

TEST_TEAR_DOWN(fb_grant) { teardown(); }

TEST(fb_grant, MissingScreensClauseFailsLaunch) {
    TEST_ASSERT_NULL(VfsFbInit(NULL, NULL));
    TEST_ASSERT_NULL(VfsFbInit(NULL, ""));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens="));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screen=main"));
}

TEST(fb_grant, UnknownScreenFailsLaunch) {
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=nope"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main,nope"));
}

TEST(fb_grant, RepeatedOrEmptyScreenFailsLaunch) {
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main,main"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main,,aux"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main,"));
}

TEST_GROUP_RUNNER(fb_grant) {
    RUN_TEST_CASE(fb_grant, MissingScreensClauseFailsLaunch);
    RUN_TEST_CASE(fb_grant, UnknownScreenFailsLaunch);
    RUN_TEST_CASE(fb_grant, RepeatedOrEmptyScreenFailsLaunch);
}

/***************************************/
TEST_GROUP(fb_tree);
/***************************************/

TEST_SETUP(fb_tree) {
    registerScreens();
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
}

TEST_TEAR_DOWN(fb_tree) { teardown(); }

TEST(fb_tree, InfoReportsGeometryFormatAndStride) {
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/fb/main/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("8 4 rgb565 16\n", buf);
}

TEST(fb_tree, Rgb888StrideIsThreeBytesPerPixel) {
    teardown();
    registerScreens();
    TEST_ASSERT_TRUE(setupGrant("screens=aux"));
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/fb/aux/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("4 2 rgb888 12\n", buf);
}

TEST(fb_tree, InfoReadsAgainFromAFreshOffset) {
    int fd = VfsOpen(vfs, "/dev/fb/main/info", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char buf[64];
    TEST_ASSERT_EQUAL_INT(14, VfsRead(vfs, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, VfsRead(vfs, fd, buf, sizeof(buf)));
    VfsClose(vfs, fd);
}

/* An ungranted screen has no namespace at all. It is unreachable, not refused.
 */
TEST(fb_tree, UngrantedScreenIsEnoent) {
    char buf[64];
    TEST_ASSERT_EQUAL_INT(-ENOENT, readNode("/dev/fb/aux/info", buf, 64));
    TEST_ASSERT_EQUAL_INT(-ENOENT, VfsOpen(vfs, "/dev/fb/aux", VFS_O_RDONLY));
}

TEST(fb_tree, UnknownNodeIsEnoent) {
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/fb/main/palette", VFS_O_RDONLY));
}

TEST(fb_tree, RootListsExactlyTheGrantedScreens) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/fb", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("main", names);
}

/* A writer's screen holds info, data and ctl, and no damage node. */
TEST(fb_tree, WriterScreenListsInfoDataAndCtl) {
    char names[64];
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/fb/main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_STRING("info,data,ctl", names);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/fb/main/damage", VFS_O_RDONLY));
}

TEST(fb_tree, InfoRefusesWrites) {
    TEST_ASSERT_EQUAL_INT(-EPERM, writeNode("/dev/fb/main/info", "1 1"));
}

TEST_GROUP_RUNNER(fb_tree) {
    RUN_TEST_CASE(fb_tree, InfoReportsGeometryFormatAndStride);
    RUN_TEST_CASE(fb_tree, Rgb888StrideIsThreeBytesPerPixel);
    RUN_TEST_CASE(fb_tree, InfoReadsAgainFromAFreshOffset);
    RUN_TEST_CASE(fb_tree, UngrantedScreenIsEnoent);
    RUN_TEST_CASE(fb_tree, UnknownNodeIsEnoent);
    RUN_TEST_CASE(fb_tree, RootListsExactlyTheGrantedScreens);
    RUN_TEST_CASE(fb_tree, WriterScreenListsInfoDataAndCtl);
    RUN_TEST_CASE(fb_tree, InfoRefusesWrites);
}

/***************************************/
TEST_GROUP(fb_data);
/***************************************/

TEST_SETUP(fb_data) {
    registerScreens();
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
}

TEST_TEAR_DOWN(fb_data) { teardown(); }

static int openData(vfs_oflags_t flags) {
    int fd = VfsOpen(vfs, "/dev/fb/main/data", flags);
    TEST_ASSERT_TRUE(fd >= 0);
    return fd;
}

TEST(fb_data, StartsBlack) {
    uint8_t buf[MAIN_BYTES];
    memset(buf, 0xAA, sizeof(buf));
    int fd = openData(VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, VfsRead(vfs, fd, buf, sizeof(buf)));
    for (size_t i = 0; i < sizeof(buf); i++)
        TEST_ASSERT_EQUAL_UINT8(0, buf[i]);
    VfsClose(vfs, fd);
}

TEST(fb_data, WriteAdvancesTheOffsetAndReadsBack) {
    const uint8_t px[] = {1, 2, 3, 4, 5, 6};
    int fd = openData(VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(6, VfsWrite(vfs, fd, px, sizeof(px)));

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 0, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT(6, pos);

    uint8_t got[6];
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 0, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(6, VfsRead(vfs, fd, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(px, got, sizeof(px));
    VfsClose(vfs, fd);
}

TEST(fb_data, PositionalTransferLeavesTheOffsetAlone) {
    const uint8_t px[] = {9, 8, 7};
    int fd = openData(VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(3, VfsPwrite(vfs, fd, px, sizeof(px), 20));

    uint8_t got[3];
    TEST_ASSERT_EQUAL_INT(3, VfsPread(vfs, fd, got, sizeof(got), 20));
    TEST_ASSERT_EQUAL_MEMORY(px, got, sizeof(px));

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 0, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT(0, pos);
    VfsClose(vfs, fd);
}

TEST(fb_data, WriteIsTruncatedAtTheEnd) {
    uint8_t px[10];
    memset(px, 0x55, sizeof(px));
    int fd = openData(VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(4,
                          VfsPwrite(vfs, fd, px, sizeof(px), MAIN_BYTES - 4));
    VfsClose(vfs, fd);
}

TEST(fb_data, AccessPastTheEndTransfersNothing) {
    uint8_t b = 1;
    int fd = openData(VFS_O_RDWR);
    TEST_ASSERT_EQUAL_INT(0, VfsPwrite(vfs, fd, &b, 1, MAIN_BYTES));
    TEST_ASSERT_EQUAL_INT(0, VfsPread(vfs, fd, &b, 1, MAIN_BYTES + 10));
    VfsClose(vfs, fd);
}

TEST(fb_data, SeekEndReportsTheScreenSize) {
    long pos = -1;
    int fd = openData(VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(0, VfsSeek(vfs, fd, 0, VFS_SEEK_END, &pos));
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, pos);
    VfsClose(vfs, fd);
}

TEST(fb_data, StatReportsTheScreenSize) {
    vfs_stat_t st;
    int fd = openData(VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT32(MAIN_BYTES, st.size);
    VfsClose(vfs, fd);
}

TEST_GROUP_RUNNER(fb_data) {
    RUN_TEST_CASE(fb_data, StartsBlack);
    RUN_TEST_CASE(fb_data, WriteAdvancesTheOffsetAndReadsBack);
    RUN_TEST_CASE(fb_data, PositionalTransferLeavesTheOffsetAlone);
    RUN_TEST_CASE(fb_data, WriteIsTruncatedAtTheEnd);
    RUN_TEST_CASE(fb_data, AccessPastTheEndTransfersNothing);
    RUN_TEST_CASE(fb_data, SeekEndReportsTheScreenSize);
    RUN_TEST_CASE(fb_data, StatReportsTheScreenSize);
}

/***************************************/
TEST_GROUP(fb_ctl);
/***************************************/

static struct {
    int flushes;
    uint16_t x, y, w, h;
    int blanks;
    bool blankOn;
    int flushResult;
} panel;

static int panelFlush(void *backing, uint16_t x, uint16_t y, uint16_t w,
                      uint16_t h, int wakeFd) {
    (void)backing;
    (void)wakeFd;
    panel.flushes++;
    panel.x = x;
    panel.y = y;
    panel.w = w;
    panel.h = h;
    return panel.flushResult;
}

static int panelBlank(void *backing, bool on) {
    (void)backing;
    panel.blanks++;
    panel.blankOn = on;
    return 0;
}

static const fb_backing_ops_t panelOps = {panelFlush, panelBlank};

TEST_SETUP(fb_ctl) {
    memset(&panel, 0, sizeof(panel));
    fb_screen_desc_t main = {"main",           MAIN_W,    MAIN_H,
                             FB_FORMAT_RGB565, &panelOps, NULL};
    TEST_ASSERT_EQUAL_INT(0, FbScreenRegister(&main));
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
}

TEST_TEAR_DOWN(fb_ctl) { teardown(); }

TEST(fb_ctl, FlushHandsTheWholeScreenToTheBacking) {
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    TEST_ASSERT_EQUAL_INT(1, panel.flushes);
    TEST_ASSERT_EQUAL_UINT16(0, panel.x);
    TEST_ASSERT_EQUAL_UINT16(0, panel.y);
    TEST_ASSERT_EQUAL_UINT16(MAIN_W, panel.w);
    TEST_ASSERT_EQUAL_UINT16(MAIN_H, panel.h);
}

TEST(fb_ctl, FlushRectangleHandsThatRectangleToTheBacking) {
    TEST_ASSERT_EQUAL_INT(14, writeNode("/dev/fb/main/ctl", "flush 1 2 3 2\n"));
    TEST_ASSERT_EQUAL_INT(1, panel.flushes);
    TEST_ASSERT_EQUAL_UINT16(1, panel.x);
    TEST_ASSERT_EQUAL_UINT16(2, panel.y);
    TEST_ASSERT_EQUAL_UINT16(3, panel.w);
    TEST_ASSERT_EQUAL_UINT16(2, panel.h);
}

TEST(fb_ctl, MalformedLinesAreEinval) {
    const char *bad[] = {"",
                         "bogus",
                         "flush 1 2",
                         "flush a b c d",
                         "flush 0 0 0 1",
                         "flush 0 0 1 0",
                         "flush -1 0 1 1",
                         "blank",
                         "blank maybe",
                         "flush 0 0 1 1 1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_INT_MESSAGE(
            -EINVAL, writeNode("/dev/fb/main/ctl", bad[i]), bad[i]);
    TEST_ASSERT_EQUAL_INT(0, panel.flushes);
}

TEST(fb_ctl, RectangleOutsideTheScreenIsEinval) {
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          writeNode("/dev/fb/main/ctl", "flush 6 0 3 1"));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          writeNode("/dev/fb/main/ctl", "flush 0 3 1 2"));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          writeNode("/dev/fb/main/ctl", "flush 8 0 1 1"));
    TEST_ASSERT_EQUAL_INT(0, panel.flushes);
}

TEST(fb_ctl, BlankReachesTheBacking) {
    TEST_ASSERT_EQUAL_INT(8, writeNode("/dev/fb/main/ctl", "blank on"));
    TEST_ASSERT_EQUAL_INT(1, panel.blanks);
    TEST_ASSERT_TRUE(panel.blankOn);
    TEST_ASSERT_EQUAL_INT(9, writeNode("/dev/fb/main/ctl", "blank off"));
    TEST_ASSERT_FALSE(panel.blankOn);
}

TEST(fb_ctl, BackingFailureReachesTheWriter) {
    panel.flushResult = -EIO;
    TEST_ASSERT_EQUAL_INT(-EIO, writeNode("/dev/fb/main/ctl", "flush"));
}

TEST(fb_ctl, CtlIsWriteOnly) {
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EPERM, readNode("/dev/fb/main/ctl", buf, 8));
}

TEST(fb_ctl, PixelsHoldWhatTheWriterWrote) {
    uint32_t stride = 0;
    TEST_ASSERT_EQUAL_INT(2, writeNode("/dev/fb/main/data", "AB"));
    const uint8_t *px = FbScreenPixels("main", &stride);
    TEST_ASSERT_NOT_NULL(px);
    TEST_ASSERT_EQUAL_UINT32(MAIN_W * 2, stride);
    TEST_ASSERT_EQUAL_UINT8('A', px[0]);
    TEST_ASSERT_EQUAL_UINT8('B', px[1]);
}

TEST(fb_ctl, PixelsOfAnUnknownScreenAreNull) {
    uint32_t stride = 7;
    TEST_ASSERT_NULL(FbScreenPixels("nope", &stride));
    TEST_ASSERT_NULL(FbScreenPixels(NULL, &stride));
    TEST_ASSERT_NOT_NULL(FbScreenPixels("main", NULL));
}

TEST_GROUP_RUNNER(fb_ctl) {
    RUN_TEST_CASE(fb_ctl, PixelsHoldWhatTheWriterWrote);
    RUN_TEST_CASE(fb_ctl, PixelsOfAnUnknownScreenAreNull);
    RUN_TEST_CASE(fb_ctl, FlushHandsTheWholeScreenToTheBacking);
    RUN_TEST_CASE(fb_ctl, FlushRectangleHandsThatRectangleToTheBacking);
    RUN_TEST_CASE(fb_ctl, MalformedLinesAreEinval);
    RUN_TEST_CASE(fb_ctl, RectangleOutsideTheScreenIsEinval);
    RUN_TEST_CASE(fb_ctl, BlankReachesTheBacking);
    RUN_TEST_CASE(fb_ctl, BackingFailureReachesTheWriter);
    RUN_TEST_CASE(fb_ctl, CtlIsWriteOnly);
}

/***************************************/
TEST_GROUP(fb_writer);
/***************************************/

TEST_SETUP(fb_writer) { registerScreens(); }

TEST_TEAR_DOWN(fb_writer) { teardown(); }

/* One writer per screen: the second launch fails, and so does one that
 * overlaps the first on any screen. */
TEST(fb_writer, SecondWriterFailsLaunch) {
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=aux,main"));
}

/* A refused overlap must not leave the screens it did get claimed. */
TEST(fb_writer, RefusedGrantClaimsNothing) {
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=aux,main"));
    vfs_driver_t *other = VfsFbInit(NULL, "screens=aux");
    TEST_ASSERT_NOT_NULL(other);
    other->Destroy(other);
}

TEST(fb_writer, DestroyReleasesTheScreen) {
    vfs_driver_t *first = VfsFbInit(NULL, "screens=main");
    TEST_ASSERT_NOT_NULL(first);
    first->Destroy(first);

    vfs_driver_t *second = VfsFbInit(NULL, "screens=main");
    TEST_ASSERT_NOT_NULL(second);
    second->Destroy(second);
}

TEST(fb_writer, WritersOfDifferentScreensCoexist) {
    vfs_driver_t *a = VfsFbInit(NULL, "screens=main");
    vfs_driver_t *b = VfsFbInit(NULL, "screens=aux");
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    a->Destroy(a);
    b->Destroy(b);
}

TEST_GROUP_RUNNER(fb_writer) {
    RUN_TEST_CASE(fb_writer, SecondWriterFailsLaunch);
    RUN_TEST_CASE(fb_writer, RefusedGrantClaimsNothing);
    RUN_TEST_CASE(fb_writer, DestroyReleasesTheScreen);
    RUN_TEST_CASE(fb_writer, WritersOfDifferentScreensCoexist);
}

/***************************************/
TEST_GROUP(fb_observe);
/***************************************/

#ifdef CONFIG_WANTED_VFS_FB_OBSERVE

#define DAMAGE_REC 8

/* A second wapp's view of the engine: its own VFS and driver instance. */
typedef struct peer_t {
    vfs_ctx_t vfs;
    vfs_driver_t *drv;
} peer_t;

static peer_t watcher;
static peer_t watcher2;

static bool attach(peer_t *p, const char *options) {
    p->vfs = VfsInit();
    p->drv = VfsFbInit(NULL, options);
    if (p->drv == NULL) {
        VfsDestroy(&p->vfs);
        return false;
    }
    DevFs_Register(p->vfs, "fb", p->drv);
    return true;
}

static void detach(peer_t *p) {
    if (p->vfs != NULL)
        VfsDestroy(&p->vfs);
    p->drv = NULL;
}

static void drawAll(uint8_t value) {
    uint8_t px[MAIN_BYTES];
    memset(px, value, sizeof(px));
    int fd = VfsOpen(vfs, "/dev/fb/main/data", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, VfsPwrite(vfs, fd, px, sizeof(px), 0));
    VfsClose(vfs, fd);
}

static void expectRecord(const uint8_t *rec, uint16_t x, uint16_t y, uint16_t w,
                         uint16_t h) {
    const uint16_t want[4] = {x, y, w, h};
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_UINT8(want[i] & 0xFF, rec[i * 2]);
        TEST_ASSERT_EQUAL_UINT8(want[i] >> 8, rec[i * 2 + 1]);
    }
}

static int openNode(peer_t *p, const char *path, vfs_oflags_t flags) {
    int fd = VfsOpen(p->vfs, path, flags);
    TEST_ASSERT_TRUE(fd >= 0);
    return fd;
}

static int readObserved(peer_t *p, uint8_t *buf, size_t len) {
    int fd = openNode(p, "/dev/fb/main/data", VFS_O_RDONLY);
    int n = VfsRead(p->vfs, fd, buf, len);
    VfsClose(p->vfs, fd);
    return n;
}

TEST_SETUP(fb_observe) {
    registerScreens();
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));
}

TEST_TEAR_DOWN(fb_observe) {
    detach(&watcher);
    detach(&watcher2);
    teardown();
}

TEST(fb_observe, ObserverTreeHoldsInfoDataAndDamage) {
    char names[64];
    vfs_ctx_t saved = vfs;
    vfs = watcher.vfs;
    TEST_ASSERT_EQUAL_INT(0, listDir("/dev/fb/main", names, sizeof(names)));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/dev/fb/main/ctl", VFS_O_WRONLY));
    vfs = saved;
    TEST_ASSERT_EQUAL_STRING("info,data,damage", names);
}

TEST(fb_observe, ObserverDoesNotCountAgainstTheWriter) {
    TEST_ASSERT_TRUE(attach(&watcher2, "screens=main,observe"));
}

TEST(fb_observe, ObserverMayLaunchBeforeTheWriter) {
    detach(&watcher);
    teardown();
    registerScreens();
    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));
    TEST_ASSERT_TRUE(setupGrant("screens=main"));
}

TEST(fb_observe, FlushReportsItsRectangle) {
    TEST_ASSERT_EQUAL_INT(14, writeNode("/dev/fb/main/ctl", "flush 1 1 2 2\n"));

    uint8_t rec[DAMAGE_REC];
    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    expectRecord(rec, 1, 1, 2, 2);
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, FullFlushReportsTheWholeScreen) {
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));

    uint8_t rec[DAMAGE_REC];
    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    expectRecord(rec, 0, 0, MAIN_W, MAIN_H);
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, UnflushedWritesNeverReachTheObserver) {
    uint8_t got[MAIN_BYTES];
    uint8_t zero[MAIN_BYTES] = {0};

    drawAll(0xAB);
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, readObserved(&watcher, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(zero, got, sizeof(got));

    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    drawAll(0xCD);
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, readObserved(&watcher, got, sizeof(got)));
    for (size_t i = 0; i < sizeof(got); i++)
        TEST_ASSERT_EQUAL_UINT8(0xAB, got[i]);
}

TEST(fb_observe, OnlyTheFlushedRectangleReachesTheObserver) {
    uint8_t got[MAIN_BYTES];

    drawAll(0x11);
    TEST_ASSERT_EQUAL_INT(13, writeNode("/dev/fb/main/ctl", "flush 2 1 2 1"));
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, readObserved(&watcher, got, sizeof(got)));
    for (size_t i = 0; i < sizeof(got); i++) {
        size_t row = i / (MAIN_W * 2);
        size_t col = (i % (MAIN_W * 2)) / 2;
        bool inside = row == 1 && col >= 2 && col < 4;
        TEST_ASSERT_EQUAL_UINT8(inside ? 0x11 : 0x00, got[i]);
    }
}

/* The group's observer is detached first, so each test starts with no flushed
 * copy and the writer's pixels as the only record of the screen. */
static void expectObserved(peer_t *p, uint8_t value) {
    uint8_t got[MAIN_BYTES];
    TEST_ASSERT_EQUAL_INT(MAIN_BYTES, readObserved(p, got, sizeof(got)));
    for (size_t i = 0; i < sizeof(got); i++)
        TEST_ASSERT_EQUAL_UINT8(value, got[i]);
}

TEST(fb_observe, LateObserverSeesWhatWasFlushedBeforeItAttached) {
    detach(&watcher);
    drawAll(0xAB);
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));

    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));
    expectObserved(&watcher, 0xAB);
}

TEST(fb_observe, AnAttachTimeSnapshotIncludesWritesNotYetFlushed) {
    detach(&watcher);
    drawAll(0x5A);

    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));
    expectObserved(&watcher, 0x5A);
}

TEST(fb_observe, WritesAfterAttachStillWaitForAFlush) {
    detach(&watcher);
    drawAll(0x11);
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));

    drawAll(0x22);
    expectObserved(&watcher, 0x11);
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    expectObserved(&watcher, 0x22);
}

TEST(fb_observe, ASecondObserverSeesTheFlushedCopyNotTheLivePixels) {
    drawAll(0x11);
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    drawAll(0x22);

    TEST_ASSERT_TRUE(attach(&watcher2, "screens=main,observe"));
    expectObserved(&watcher2, 0x11);
}

TEST(fb_observe, TheCopyStartsAgainFromTheLivePixelsWhenTheLastObserverLeaves) {
    drawAll(0x11);
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    detach(&watcher);
    drawAll(0x33);

    TEST_ASSERT_TRUE(attach(&watcher, "screens=main,observe"));
    expectObserved(&watcher, 0x33);
}

TEST(fb_observe, ObserverCannotWrite) {
    int fd = openNode(&watcher, "/dev/fb/main/data", VFS_O_RDWR);
    uint8_t b = 1;
    TEST_ASSERT_EQUAL_INT(-EPERM, VfsWrite(watcher.vfs, fd, &b, 1));
    TEST_ASSERT_EQUAL_INT(-EPERM, VfsPwrite(watcher.vfs, fd, &b, 1, 0));
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, NonBlockingDamageReadIsEagainWhenIdle) {
    uint8_t rec[DAMAGE_REC];
    int fd = openNode(&watcher, "/dev/fb/main/damage",
                      VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, DamageReadsWholeRecordsOnly) {
    uint8_t rec[DAMAGE_REC * 2];
    TEST_ASSERT_EQUAL_INT(13, writeNode("/dev/fb/main/ctl", "flush 0 0 1 1"));
    TEST_ASSERT_EQUAL_INT(13, writeNode("/dev/fb/main/ctl", "flush 1 0 1 1"));

    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          VfsRead(watcher.vfs, fd, rec, DAMAGE_REC - 1));
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC, VfsRead(watcher.vfs, fd, rec, 12));
    expectRecord(rec, 0, 0, 1, 1);
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    expectRecord(rec, 1, 0, 1, 1);
    VfsClose(watcher.vfs, fd);
}

/* An observer that falls behind gets one full-screen record, never a gap. */
TEST(fb_observe, OverflowCollapsesToOneFullScreenRecord) {
    for (int i = 0; i < CONFIG_WANTED_FB_DAMAGE_QUEUE + 1; i++)
        TEST_ASSERT_EQUAL_INT(13,
                              writeNode("/dev/fb/main/ctl", "flush 0 0 1 1"));

    uint8_t rec[DAMAGE_REC * 2];
    int fd = openNode(&watcher, "/dev/fb/main/damage",
                      VFS_O_RDONLY | VFS_O_NONBLOCK);
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    expectRecord(rec, 0, 0, MAIN_W, MAIN_H);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, EachObserverKeepsItsOwnQueue) {
    TEST_ASSERT_TRUE(attach(&watcher2, "screens=main,observe"));
    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));

    uint8_t rec[DAMAGE_REC];
    int a = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    int b = openNode(&watcher2, "/dev/fb/main/damage", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher.vfs, a, rec, sizeof(rec)));
    TEST_ASSERT_EQUAL_INT(DAMAGE_REC,
                          VfsRead(watcher2.vfs, b, rec, sizeof(rec)));
    VfsClose(watcher.vfs, a);
    VfsClose(watcher2.vfs, b);
}

TEST(fb_observe, PollReportsPendingDamage) {
    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    uint32_t avail = 99;
    TEST_ASSERT_EQUAL_INT(0, VfsPoll(watcher.vfs, fd, &avail));

    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    TEST_ASSERT_EQUAL_INT(VFS_POLL_IN, VfsPoll(watcher.vfs, fd, &avail));
    TEST_ASSERT_EQUAL_UINT32(DAMAGE_REC, avail);
    VfsClose(watcher.vfs, fd);
}

/* A stop raises the wake descriptor, which ends a blocked damage read. */
TEST(fb_observe, StopInterruptsABlockedDamageRead) {
    int wake = PlatformWakeCreate();
    TEST_ASSERT_TRUE(wake >= 0);
    watcher.drv->SetWake(watcher.drv->ctx, wake);
    VfsSetWakeFd(watcher.vfs, wake);
    PlatformWakeRaise(wake);

    uint8_t rec[DAMAGE_REC];
    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EINTR, VfsRead(watcher.vfs, fd, rec, sizeof(rec)));
    VfsClose(watcher.vfs, fd);
}

static __wasi_subscription_t damageSub(int fd) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = 1;
    s.type = __WASI_EVENTTYPE_FD_READ;
    s.u.fd_readwrite.fd = (uint32_t)fd;
    return s;
}

static __wasi_subscription_t clockSub(uint64_t timeoutNs) {
    __wasi_subscription_t s;
    memset(&s, 0, sizeof(s));
    s.userdata = 2;
    s.type = __WASI_EVENTTYPE_CLOCK;
    s.u.clock.id = 1; /* monotonic */
    s.u.clock.timeout = timeoutNs;
    return s;
}

/* One poll_oneoff waits on damage beside another source and wakes on a flush.
 */
TEST(fb_observe, PollOneoffWakesOnAFlush) {
    int fd = openNode(&watcher, "/dev/fb/main/damage", VFS_O_RDONLY);
    __wasi_subscription_t subs[2] = {damageSub(fd), clockSub(2000000)};
    __wasi_event_t ev[2];
    uint32_t n = 0;

    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(watcher.vfs, subs, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_CLOCK, ev[0].type);

    TEST_ASSERT_EQUAL_INT(5, writeNode("/dev/fb/main/ctl", "flush"));
    subs[1] = clockSub(5000000000ULL);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS,
                             WasiPollOneoff(watcher.vfs, subs, ev, 2, &n));
    TEST_ASSERT_EQUAL_UINT32(1, n);
    TEST_ASSERT_EQUAL_UINT8(__WASI_EVENTTYPE_FD_READ, ev[0].type);
    TEST_ASSERT_EQUAL_UINT16(__WASI_ERRNO_SUCCESS, ev[0].error);
    TEST_ASSERT_EQUAL_UINT64(DAMAGE_REC, ev[0].fd_readwrite.nbytes);
    VfsClose(watcher.vfs, fd);
}

TEST(fb_observe, ObserverLimitFailsTheLaunchPastIt) {
    peer_t extra[CONFIG_WANTED_FB_MAX_OBSERVERS];
    memset(extra, 0, sizeof(extra));
    int held = 1;

    for (int i = 0; held < CONFIG_WANTED_FB_MAX_OBSERVERS; i++, held++)
        TEST_ASSERT_TRUE(attach(&extra[i], "screens=main,observe"));
    TEST_ASSERT_FALSE(attach(&extra[held - 1], "screens=main,observe"));

    for (int i = 0; i < held - 1; i++)
        detach(&extra[i]);
}

#endif /* CONFIG_WANTED_VFS_FB_OBSERVE */

#ifndef CONFIG_WANTED_VFS_FB_OBSERVE
TEST_SETUP(fb_observe) { registerScreens(); }

TEST_TEAR_DOWN(fb_observe) { teardown(); }

/* A build without observers never reads an observe grant as a writer's. */
TEST(fb_observe, ObserveGrantFailsLaunchWithoutObservers) {
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=main,observe"));
    TEST_ASSERT_NULL(VfsFbInit(NULL, "screens=observe"));
    vfs_driver_t *w = VfsFbInit(NULL, "screens=main");
    TEST_ASSERT_NOT_NULL(w);
    w->Destroy(w);
}
#endif

TEST_GROUP_RUNNER(fb_observe) {
#ifdef CONFIG_WANTED_VFS_FB_OBSERVE
    RUN_TEST_CASE(fb_observe, ObserverTreeHoldsInfoDataAndDamage);
    RUN_TEST_CASE(fb_observe, ObserverDoesNotCountAgainstTheWriter);
    RUN_TEST_CASE(fb_observe, ObserverMayLaunchBeforeTheWriter);
    RUN_TEST_CASE(fb_observe, FlushReportsItsRectangle);
    RUN_TEST_CASE(fb_observe, FullFlushReportsTheWholeScreen);
    RUN_TEST_CASE(fb_observe, UnflushedWritesNeverReachTheObserver);
    RUN_TEST_CASE(fb_observe, OnlyTheFlushedRectangleReachesTheObserver);
    RUN_TEST_CASE(fb_observe, LateObserverSeesWhatWasFlushedBeforeItAttached);
    RUN_TEST_CASE(fb_observe, AnAttachTimeSnapshotIncludesWritesNotYetFlushed);
    RUN_TEST_CASE(fb_observe, WritesAfterAttachStillWaitForAFlush);
    RUN_TEST_CASE(fb_observe,
                  ASecondObserverSeesTheFlushedCopyNotTheLivePixels);
    RUN_TEST_CASE(fb_observe,
                  TheCopyStartsAgainFromTheLivePixelsWhenTheLastObserverLeaves);
    RUN_TEST_CASE(fb_observe, ObserverCannotWrite);
    RUN_TEST_CASE(fb_observe, NonBlockingDamageReadIsEagainWhenIdle);
    RUN_TEST_CASE(fb_observe, DamageReadsWholeRecordsOnly);
    RUN_TEST_CASE(fb_observe, OverflowCollapsesToOneFullScreenRecord);
    RUN_TEST_CASE(fb_observe, EachObserverKeepsItsOwnQueue);
    RUN_TEST_CASE(fb_observe, PollReportsPendingDamage);
    RUN_TEST_CASE(fb_observe, StopInterruptsABlockedDamageRead);
    RUN_TEST_CASE(fb_observe, PollOneoffWakesOnAFlush);
    RUN_TEST_CASE(fb_observe, ObserverLimitFailsTheLaunchPastIt);
#else
    RUN_TEST_CASE(fb_observe, ObserveGrantFailsLaunchWithoutObservers);
#endif
}

/***************************************/
TEST_GROUP(fb_config);
/***************************************/

#define SCREEN_CFG(list) "{\"system\":{\"screens\":" list "}}"

TEST_SETUP(fb_config) {}

TEST_TEAR_DOWN(fb_config) { teardown(); }

static int parseScreens(const char *json) {
    return WantedParseConfig(json, strlen(json));
}

TEST(fb_config, SystemConfigDeclaresAScreen) {
    TEST_ASSERT_EQUAL_INT(
        0, parseScreens(SCREEN_CFG("[{\"name\":\"lcd\",\"width\":16,"
                                   "\"height\":8,\"format\":\"rgb565\"}]")));
    TEST_ASSERT_TRUE(setupGrant("screens=lcd"));
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/fb/lcd/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("16 8 rgb565 32\n", buf);
}

TEST(fb_config, SystemConfigDeclaresAnRgb888Screen) {
    TEST_ASSERT_EQUAL_INT(
        0, parseScreens(SCREEN_CFG("[{\"name\":\"lcd\",\"width\":5,"
                                   "\"height\":3,\"format\":\"rgb888\"}]")));
    TEST_ASSERT_TRUE(setupGrant("screens=lcd"));
    char buf[64];
    TEST_ASSERT_GREATER_THAN_INT(0, readNode("/dev/fb/lcd/info", buf, 64));
    TEST_ASSERT_EQUAL_STRING("5 3 rgb888 15\n", buf);
}

/* A declaration the engine cannot honour fails the parse; nothing is skipped.
 */
TEST(fb_config, MalformedDeclarationFailsTheParse) {
    const char *bad[] = {
        SCREEN_CFG("{}"),
        SCREEN_CFG("[{\"width\":4,\"height\":4,\"format\":\"rgb565\"}]"),
        SCREEN_CFG("[{\"name\":\"a\",\"height\":4,\"format\":\"rgb565\"}]"),
        SCREEN_CFG("[{\"name\":\"a\",\"width\":0,\"height\":4,"
                   "\"format\":\"rgb565\"}]"),
        SCREEN_CFG("[{\"name\":\"a\",\"width\":4,\"height\":4,"
                   "\"format\":\"mono\"}]"),
        SCREEN_CFG("[{\"name\":\"a\",\"width\":4,\"height\":4}]"),
        SCREEN_CFG("[{\"name\":\"a b\",\"width\":4,\"height\":4,"
                   "\"format\":\"rgb565\"}]"),
        SCREEN_CFG("[{\"name\":\"observe\",\"width\":4,\"height\":4,"
                   "\"format\":\"rgb565\"}]"),
        SCREEN_CFG("[{\"name\":\"a\",\"width\":70000,\"height\":4,"
                   "\"format\":\"rgb565\"}]"),
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(parseScreens(bad[i]) != 0, bad[i]);
        FbScreensReset();
    }
}

TEST(fb_config, DeclaringTheSameScreenAgainIsAccepted) {
    const char *cfg = SCREEN_CFG("[{\"name\":\"lcd\",\"width\":4,"
                                 "\"height\":4,\"format\":\"rgb565\"}]");
    TEST_ASSERT_EQUAL_INT(0, parseScreens(cfg));
    TEST_ASSERT_EQUAL_INT(0, parseScreens(cfg));
}

TEST(fb_config, DeclaringAScreenDifferentlyFailsTheParse) {
    TEST_ASSERT_EQUAL_INT(
        0, parseScreens(SCREEN_CFG("[{\"name\":\"lcd\",\"width\":4,"
                                   "\"height\":4,\"format\":\"rgb565\"}]")));
    TEST_ASSERT_TRUE(
        parseScreens(SCREEN_CFG("[{\"name\":\"lcd\",\"width\":8,"
                                "\"height\":4,\"format\":\"rgb565\"}]")) != 0);
}

/* Naming fb in a launch config reaches this driver: it is in the core table. */
TEST(fb_config, DriverIsListedAmongTheGrantableDrivers) {
    char names[512];
    TEST_ASSERT_GREATER_THAN_INT(0, WantedListDrivers(names, sizeof(names)));
    TEST_ASSERT_NOT_NULL(strstr(names, "fb"));
}

TEST_GROUP_RUNNER(fb_config) {
    RUN_TEST_CASE(fb_config, DriverIsListedAmongTheGrantableDrivers);
    RUN_TEST_CASE(fb_config, SystemConfigDeclaresAScreen);
    RUN_TEST_CASE(fb_config, SystemConfigDeclaresAnRgb888Screen);
    RUN_TEST_CASE(fb_config, MalformedDeclarationFailsTheParse);
    RUN_TEST_CASE(fb_config, DeclaringTheSameScreenAgainIsAccepted);
    RUN_TEST_CASE(fb_config, DeclaringAScreenDifferentlyFailsTheParse);
}
