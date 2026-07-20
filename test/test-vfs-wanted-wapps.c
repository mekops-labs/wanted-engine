/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "test-utils.h"

#include <platform.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_wanted_wapps — the decomposed per-wapp control namespace
 * (vfs-wanted-wapps.c) driven directly through its vtable, backed by the dummy
 * in-memory wapp-state and registry mocks. Path carries identity; reads are
 * plain text; the start config is the only JSON.
 * ═══════════════════════════════════════════════════════════════════════════ */

extern const vfs_driver_t WantedWappsDriver; /* mounted at /dev/wanted/wapps */
extern const vfs_driver_t WantedCtlDriver;   /* mounted at /dev/wanted/ctl   */

static const vfs_driver_t *drv;
static const vfs_driver_t *ctl;

static wapp_state_t MakeState(const char *name, uint8_t id, status_t status,
                              uint8_t major, uint8_t minor, uint8_t patch,
                              uint8_t pkg) {
    wapp_state_t s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, name, WAPP_MAX_NAME_LEN - 1);
    s.id = id;
    s.status = status;
    /* version is an opaque tag string. */
    snprintf(s.version, WAPP_MAX_VERSION_LEN, "%u.%u.%u-%u", major, minor, patch,
             pkg);
    return s;
}

/* ── wapps/ directory + per-wapp reads ──────────────────────────────────── */

TEST_GROUP(vfs_wanted_wapps);

TEST_SETUP(vfs_wanted_wapps) {
    DummyWappStateReset();
    DummyRegistryReset();
    drv = &WantedWappsDriver;
    ctl = &WantedCtlDriver;
    drv->Destroy((vfs_driver_t *)drv); /* clear fd table + pending configs */
}

TEST_TEAR_DOWN(vfs_wanted_wapps) {
    drv->Destroy((vfs_driver_t *)drv);
}

static int OpenLeaf(const char *path) {
    return drv->Open(drv->ctx, path, VFS_O_RDONLY);
}

/* Reserve a wapp namespace via the root ctl `create` verb, the way a wapp comes
 * into being. The per-wapp nodes are only openable once the wapp is known
 * (created or live), so most tests create or seed their wapp first. */
static void CreateWapp(const char *name) {
    char cmd[64];
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    int n = snprintf(cmd, sizeof(cmd), "create %s", name);
    ctl->Write(ctl->ctx, fd, cmd, (size_t)n);
}

/* enumeration */

TEST(vfs_wanted_wapps, ReadDirRoot_EnumeratesKnownWapps) {
    wapp_state_t seed[2] = {
        MakeState("alpha", 1, RUNNING, 1, 0, 0, 0),
        MakeState("bravo", 2, EXITED, 2, 0, 0, 0),
    };
    DummyWappStateSeed(seed, 2);

    int fd = OpenLeaf("/");
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, used, "alpha", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "bravo", 5));
}

TEST(vfs_wanted_wapps, ReadDirWapp_ListsControlFiles) {
    CreateWapp("alpha");
    int fd = OpenLeaf("alpha");
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "ctl", 3));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "state", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "config", 6));
    /* Observability reads moved to /proc/wapps; logs are a mountable driver. */
    TEST_ASSERT_FALSE(HasBytes(buf, used, "version", 7));
    TEST_ASSERT_FALSE(HasBytes(buf, used, "log", 3));
}

TEST(vfs_wanted_wapps, Stat_RootIsDir_LeafIsCharDevice) {
    CreateWapp("alpha");
    vfs_stat_t st;
    int d = OpenLeaf("alpha");
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, d, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    int l = OpenLeaf("alpha/state");
    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, l, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, st.filetype);
}

TEST(vfs_wanted_wapps, OpenUnknownLeaf_ReturnsEnoent) {
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("alpha/bogus"));
}

/* plain-text reads */

TEST(vfs_wanted_wapps, ReadState_Running) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 2, 3, 4);
    DummyWappStateSeed(&seed, 1);

    int fd = OpenLeaf("alpha/state");
    char buf[32] = {0};
    int n = drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("running", buf);
    TEST_ASSERT_EQUAL_INT(7, n);
}

/* An unknown wapp has no namespace: opening any node under it is ENOENT, so a
 * name cannot be probed by guessing its path. */
TEST(vfs_wanted_wapps, OpenUnknownWapp_ReturnsEnoent) {
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("ghost"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("ghost/state"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("ghost/config"));
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("ghost/ctl"));
}

TEST(vfs_wanted_wapps, ReadEof_IsPerFd) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    /* Two independent fds on the same node. Draining one to EOF must not
     * latch EOF on the other — each fd keeps an independent read cursor. */
    int a = OpenLeaf("alpha/state");
    int b = OpenLeaf("alpha/state");
    char buf[32];

    TEST_ASSERT_TRUE(drv->Read(drv->ctx, a, buf, sizeof(buf)) > 0);
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, a, buf, sizeof(buf))); /* EOF */
    TEST_ASSERT_TRUE(drv->Read(drv->ctx, b, buf, sizeof(buf)) > 0); /* fresh */
}

TEST(vfs_wanted_wapps, ReadWriteOnlyNode_ReturnsEinval) {
    CreateWapp("alpha");
    int fd = OpenLeaf("alpha/ctl");
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Read(drv->ctx, fd, buf, sizeof(buf)));
}

/* ctl verb */

TEST(vfs_wanted_wapps, CtlStop_TransitionsState) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(4, drv->Write(drv->ctx, c, "stop", 4));

    int s = OpenLeaf("alpha/state");
    char buf[32] = {0};
    drv->Read(drv->ctx, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("exited", buf);
}

TEST(vfs_wanted_wapps, CtlStop_TrimsTrailingNewline) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(5, drv->Write(drv->ctx, c, "stop\n", 5));

    int s = OpenLeaf("alpha/state");
    char buf[32] = {0};
    drv->Read(drv->ctx, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("exited", buf);
}

TEST(vfs_wanted_wapps, CtlUnknownVerb_ReturnsEinval) {
    CreateWapp("alpha");
    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Write(drv->ctx, c, "frobnicate", 10));
}

TEST(vfs_wanted_wapps, CtlStart_NoImageReturnsEnosys) {
    /* A live wapp (no pending reservation) passes the config gate, so start
     * reaches the loader; the dummy cannot map an image and returns -ENOSYS.
     * Identity comes from the path; the verb is just "start". */
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);
    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, c, "start", 5));
}

TEST(vfs_wanted_wapps, CtlOversizedWrite_ReturnsEmsgsize) {
    CreateWapp("alpha");
    int c = OpenLeaf("alpha/ctl");
    char big[64];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE,
                          drv->Write(drv->ctx, c, big, sizeof(big)));
}

/* config + root ctl */

TEST(vfs_wanted_wapps, ConfigWrite_ParsesJson) {
    CreateWapp("alpha");
    int c = OpenLeaf("alpha/config");
    const char *cfg =
        "{\"drivers\":[{\"name\":\"null\",\"path\":\"/dev/null\"}],"
        "\"preopens\":[\"/var/lib/x\"]}";
    TEST_ASSERT_EQUAL_INT((int)strlen(cfg),
                          drv->Write(drv->ctx, c, cfg, strlen(cfg)));
}

TEST(vfs_wanted_wapps, ConfigWrite_MalformedJsonRejected) {
    CreateWapp("alpha");
    int c = OpenLeaf("alpha/config");
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, c, "not json", 8) < 0);
}

/* The root ctl does not launch wapps — `start` is not a root verb; a wapp is
 * started through its own wapps/<name>/ctl (see CtlStart_NoImageReturnsEnosys). */
TEST(vfs_wanted_wapps, RootCtlStartRejected_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          ctl->Write(ctl->ctx, fd, "start alpha", 11));
}

TEST(vfs_wanted_wapps, RootCtlGarbage_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Write(ctl->ctx, fd, "frob", 4));
}

TEST(vfs_wanted_wapps, RootCtlRead_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_RDONLY);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Read(ctl->ctx, fd, buf, sizeof(buf)));
}

/* create — first-start lifecycle */

TEST(vfs_wanted_wapps, RootCtlCreate_MakesWappCreatedAndEnumerable) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(12, ctl->Write(ctl->ctx, fd, "create ghost", 12));

    /* A created-but-not-started wapp reports the CREATED state. */
    int s = OpenLeaf("ghost/state");
    char buf[32] = {0};
    drv->Read(drv->ctx, s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("created", buf);

    /* ...and enumerates under wapps/ before it ever runs. */
    int dd = OpenLeaf("/");
    uint8_t db[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, dd, db, sizeof(db), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(db, used, "ghost", 5));
}

TEST(vfs_wanted_wapps, ConfigWriteAfterCreate_TransitionsToNotStarted) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create ghost", 12) > 0);

    /* A bare reservation (no config yet) reads created. */
    char buf[32] = {0};
    drv->Read(drv->ctx, OpenLeaf("ghost/state"), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("created", buf);

    /* Writing the config applies it and moves the wapp to not_started. */
    const char *cfg = "{\"args\":[\"x\"]}";
    int c = OpenLeaf("ghost/config");
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, c, cfg, strlen(cfg)) > 0);

    memset(buf, 0, sizeof(buf));
    drv->Read(drv->ctx, OpenLeaf("ghost/state"), buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("not_started", buf);
}

/* A bare `create` reservation has no config; starting it is rejected — config
 * must be written first (created cannot transition straight to starting). A
 * name with no reservation is unaffected (see CtlStart_NoImageReturnsEnosys,
 * which reaches the loader and fails on the missing image instead). */
TEST(vfs_wanted_wapps, StartCreatedWithoutConfig_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create ghost", 12) > 0);

    int c = OpenLeaf("ghost/ctl");
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Write(drv->ctx, c, "start", 5));
}

/* `start <image>` on a bare `create` reservation (no config written) satisfies
 * the start gate on its own — the explicit image is enough to launch, so the
 * loader is reached (dummy can't map an image → -ENOSYS) rather than the
 * unconfigured-reservation -EINVAL a bare `start` would get. */
TEST(vfs_wanted_wapps, CtlStartWithImage_CreatedWithoutConfig_ReachesLoader) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create ghost", 12) > 0);

    int c = OpenLeaf("ghost/ctl");
    TEST_ASSERT_EQUAL_INT(-ENOSYS,
                          drv->Write(drv->ctx, c, "start duplex", 12));
}

/* Two distinct instances can be created and bound to a single image via config
 * `image`; each start reaches the loader independently (dummy → -ENOSYS). This
 * is the multi-instance path: one image, N instance names. */
TEST(vfs_wanted_wapps, MultiInstance_TwoInstancesOneImage) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create reader", 13) > 0);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create writer", 13) > 0);

    const char *cfg = "{\"image\":\"duplex\"}";
    int rc = OpenLeaf("reader/config");
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, rc, cfg, strlen(cfg)) > 0);
    int wc = OpenLeaf("writer/config");
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, wc, cfg, strlen(cfg)) > 0);

    int r = OpenLeaf("reader/ctl");
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, r, "start", 5));
    int w = OpenLeaf("writer/ctl");
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, w, "start", 5));
}

TEST(vfs_wanted_wapps, RootCtlCreateNoName_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Write(ctl->ctx, fd, "create ", 7));
}

TEST(vfs_wanted_wapps, RootCtlCreateTooLongName_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    /* 16-char name exceeds WAPP_MAX_NAME_LEN (15). */
    const char *cmd = "create aaaaaaaaaaaaaaaa";
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          ctl->Write(ctl->ctx, fd, cmd, strlen(cmd)));
}

/* Write "create w<i>" and return the driver's result. The slot pool is sized by
 * configuration, so these tests count against CONFIG_WANTED_MAX_WAPPS rather
 * than a literal — a build with a different envelope must still exhaust at its
 * own limit, not at the default one. */
static int CreateNth(int fd, int i) {
    char cmd[32];
    int n = snprintf(cmd, sizeof(cmd), "create w%d", i);
    return ctl->Write(ctl->ctx, fd, cmd, (size_t)n);
}

TEST(vfs_wanted_wapps, RootCtlCreateExhaustsSlots_ReturnsEnospc) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++)
        TEST_ASSERT_TRUE(CreateNth(fd, i) > 0);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, CreateNth(fd, CONFIG_WANTED_MAX_WAPPS));
}

TEST(vfs_wanted_wapps, RootCtlCreateIsIdempotent) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    /* Re-creating the same name reuses its slot rather than exhausting the
     * pool, so two more distinct names still fit. */
    TEST_ASSERT_TRUE(CreateNth(fd, 0) > 0);
    TEST_ASSERT_TRUE(CreateNth(fd, 0) > 0);
    for (int i = 1; i < CONFIG_WANTED_MAX_WAPPS; i++)
        TEST_ASSERT_TRUE(CreateNth(fd, i) > 0);
}

/* delete — slot release */

/* Deleting a `create` reservation removes the namespace: the name stops being
 * known, so its nodes return ENOENT again. */
TEST(vfs_wanted_wapps, RootCtlDelete_RemovesReservation) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_TRUE(ctl->Write(ctl->ctx, fd, "create ghost", 12) > 0);
    TEST_ASSERT_TRUE(OpenLeaf("ghost/state") >= 0); /* known after create */

    TEST_ASSERT_EQUAL_INT(12, ctl->Write(ctl->ctx, fd, "delete ghost", 12));

    /* Gone: the per-wapp nodes are ENOENT and it no longer enumerates. */
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("ghost/state"));
    int dd = OpenLeaf("/");
    uint8_t db[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, dd, db, sizeof(db), &cookie, &used));
    TEST_ASSERT_FALSE(HasBytes(db, used, "ghost", 5));
}

/* Deleting a terminal (exited) wapp releases its platform slot; the name then
 * returns ENOENT. */
TEST(vfs_wanted_wapps, RootCtlDelete_ReleasesTerminalSlot) {
    wapp_state_t seed = MakeState("alpha", 1, EXITED, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(12, ctl->Write(ctl->ctx, fd, "delete alpha", 12));
    TEST_ASSERT_EQUAL_INT(-ENOENT, OpenLeaf("alpha/state"));
}

/* A running wapp cannot be deleted — it must be stopped first. The slot is left
 * intact (-EBUSY) and the wapp stays enumerable. */
TEST(vfs_wanted_wapps, RootCtlDelete_RunningRejectedEbusy) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EBUSY, ctl->Write(ctl->ctx, fd, "delete alpha", 12));
    TEST_ASSERT_TRUE(OpenLeaf("alpha/state") >= 0); /* still known */
}

TEST(vfs_wanted_wapps, RootCtlDelete_UnknownReturnsEnoent) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, ctl->Write(ctl->ctx, fd, "delete ghost", 12));
}

TEST(vfs_wanted_wapps, RootCtlDeleteNoName_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Write(ctl->ctx, fd, "delete ", 7));
}

/* delete frees a pending slot back to the pool, so a new name fits afterwards. */
TEST(vfs_wanted_wapps, RootCtlDelete_FreesPoolSlot) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++)
        TEST_ASSERT_TRUE(CreateNth(fd, i) > 0);
    TEST_ASSERT_EQUAL_INT(-ENOSPC, CreateNth(fd, CONFIG_WANTED_MAX_WAPPS));

    TEST_ASSERT_EQUAL_INT(9, ctl->Write(ctl->ctx, fd, "delete w0", 9));
    TEST_ASSERT_TRUE(CreateNth(fd, CONFIG_WANTED_MAX_WAPPS) > 0);
}

/* fd table exhaustion — bounded, no overflow */

TEST(vfs_wanted_wapps, OpenExhaustsFdTable_ReturnsEmfile) {
    CreateWapp("alpha");
    int last = 0;
    for (int i = 0; i < 16; i++)
        last = OpenLeaf("alpha/state");
    TEST_ASSERT_EQUAL_INT(-EMFILE, last);
}

TEST_GROUP_RUNNER(vfs_wanted_wapps) {
    RUN_TEST_CASE(vfs_wanted_wapps, ReadDirRoot_EnumeratesKnownWapps);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadDirWapp_ListsControlFiles);
    RUN_TEST_CASE(vfs_wanted_wapps, Stat_RootIsDir_LeafIsCharDevice);
    RUN_TEST_CASE(vfs_wanted_wapps, OpenUnknownLeaf_ReturnsEnoent);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadState_Running);
    RUN_TEST_CASE(vfs_wanted_wapps, OpenUnknownWapp_ReturnsEnoent);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadEof_IsPerFd);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadWriteOnlyNode_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStop_TransitionsState);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStop_TrimsTrailingNewline);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlUnknownVerb_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStart_NoImageReturnsEnosys);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlOversizedWrite_ReturnsEmsgsize);
    RUN_TEST_CASE(vfs_wanted_wapps, ConfigWrite_ParsesJson);
    RUN_TEST_CASE(vfs_wanted_wapps, ConfigWrite_MalformedJsonRejected);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlStartRejected_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlGarbage_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlRead_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlCreate_MakesWappCreatedAndEnumerable);
    RUN_TEST_CASE(vfs_wanted_wapps, ConfigWriteAfterCreate_TransitionsToNotStarted);
    RUN_TEST_CASE(vfs_wanted_wapps, StartCreatedWithoutConfig_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps,
                  CtlStartWithImage_CreatedWithoutConfig_ReachesLoader);
    RUN_TEST_CASE(vfs_wanted_wapps, MultiInstance_TwoInstancesOneImage);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlCreateNoName_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlCreateTooLongName_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlCreateExhaustsSlots_ReturnsEnospc);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlCreateIsIdempotent);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDelete_RemovesReservation);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDelete_ReleasesTerminalSlot);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDelete_RunningRejectedEbusy);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDelete_UnknownReturnsEnoent);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDeleteNoName_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlDelete_FreesPoolSlot);
    RUN_TEST_CASE(vfs_wanted_wapps, OpenExhaustsFdTable_ReturnsEmfile);
}
