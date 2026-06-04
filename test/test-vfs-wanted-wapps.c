#include "unity_fixture.h"

#include <errno.h>
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
    s.version.v[0] = major;
    s.version.v[1] = minor;
    s.version.v[2] = patch;
    s.version.v[3] = pkg;
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

/* M0 — enumeration */

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
    int fd = OpenLeaf("alpha");
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "ctl", 3));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "state", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "version", 7));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "id", 2));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "config", 6));
}

TEST(vfs_wanted_wapps, Stat_RootIsDir_LeafIsCharDevice) {
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

/* M1 — plain-text reads */

TEST(vfs_wanted_wapps, ReadState_Running) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 2, 3, 4);
    DummyWappStateSeed(&seed, 1);

    int fd = OpenLeaf("alpha/state");
    char buf[32] = {0};
    int n = drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("running", buf);
    TEST_ASSERT_EQUAL_INT(7, n);
}

TEST(vfs_wanted_wapps, ReadState_UnknownWappIsNotStarted) {
    int fd = OpenLeaf("ghost/state");
    char buf[32] = {0};
    drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("not_started", buf);
}

TEST(vfs_wanted_wapps, ReadVersion_Formatted) {
    wapp_state_t seed = MakeState("alpha", 1, RUNNING, 1, 2, 3, 4);
    DummyWappStateSeed(&seed, 1);

    int fd = OpenLeaf("alpha/version");
    char buf[32] = {0};
    drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("1.2.3-4", buf);
}

TEST(vfs_wanted_wapps, ReadId_Decimal) {
    wapp_state_t seed = MakeState("alpha", 7, RUNNING, 1, 0, 0, 0);
    DummyWappStateSeed(&seed, 1);

    int fd = OpenLeaf("alpha/id");
    char buf[32] = {0};
    drv->Read(drv->ctx, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("7", buf);
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
    int fd = OpenLeaf("alpha/ctl");
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Read(drv->ctx, fd, buf, sizeof(buf)));
}

/* M2 — ctl verb */

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
    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Write(drv->ctx, c, "frobnicate", 10));
}

TEST(vfs_wanted_wapps, CtlStart_NoImageReturnsEnosys) {
    /* The dummy cannot load a WASM image: PlatformRegistryWappLoad -> -ENOSYS.
     * Identity comes from the path; the verb is just "start". */
    int c = OpenLeaf("alpha/ctl");
    TEST_ASSERT_EQUAL_INT(-ENOSYS, drv->Write(drv->ctx, c, "start", 5));
}

TEST(vfs_wanted_wapps, CtlOversizedWrite_ReturnsEmsgsize) {
    int c = OpenLeaf("alpha/ctl");
    char big[64];
    memset(big, 'x', sizeof(big));
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE,
                          drv->Write(drv->ctx, c, big, sizeof(big)));
}

/* M3 — config + root ctl */

TEST(vfs_wanted_wapps, ConfigWrite_ParsesJson) {
    int c = OpenLeaf("alpha/config");
    const char *cfg =
        "{\"drivers\":[{\"name\":\"null\",\"path\":\"/dev/null\"}],"
        "\"preopens\":[\"/var/lib/x\"]}";
    TEST_ASSERT_EQUAL_INT((int)strlen(cfg),
                          drv->Write(drv->ctx, c, cfg, strlen(cfg)));
}

TEST(vfs_wanted_wapps, ConfigWrite_MalformedJsonRejected) {
    int c = OpenLeaf("alpha/config");
    TEST_ASSERT_TRUE(drv->Write(drv->ctx, c, "not json", 8) < 0);
}

TEST(vfs_wanted_wapps, RootCtlStart_NoImageReturnsEnosys) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-ENOSYS,
                          ctl->Write(ctl->ctx, fd, "start alpha", 11));
}

TEST(vfs_wanted_wapps, RootCtlGarbage_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Write(ctl->ctx, fd, "frob", 4));
}

TEST(vfs_wanted_wapps, RootCtlStartNoName_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_WRONLY);
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Write(ctl->ctx, fd, "start ", 6));
}

TEST(vfs_wanted_wapps, RootCtlRead_ReturnsEinval) {
    int fd = ctl->Open(ctl->ctx, "", VFS_O_RDONLY);
    char buf[8];
    TEST_ASSERT_EQUAL_INT(-EINVAL, ctl->Read(ctl->ctx, fd, buf, sizeof(buf)));
}

/* fd table exhaustion — bounded, no overflow */

TEST(vfs_wanted_wapps, OpenExhaustsFdTable_ReturnsEmfile) {
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
    RUN_TEST_CASE(vfs_wanted_wapps, ReadState_UnknownWappIsNotStarted);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadVersion_Formatted);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadId_Decimal);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadEof_IsPerFd);
    RUN_TEST_CASE(vfs_wanted_wapps, ReadWriteOnlyNode_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStop_TransitionsState);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStop_TrimsTrailingNewline);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlUnknownVerb_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlStart_NoImageReturnsEnosys);
    RUN_TEST_CASE(vfs_wanted_wapps, CtlOversizedWrite_ReturnsEmsgsize);
    RUN_TEST_CASE(vfs_wanted_wapps, ConfigWrite_ParsesJson);
    RUN_TEST_CASE(vfs_wanted_wapps, ConfigWrite_MalformedJsonRejected);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlStart_NoImageReturnsEnosys);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlGarbage_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlStartNoName_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, RootCtlRead_ReturnsEinval);
    RUN_TEST_CASE(vfs_wanted_wapps, OpenExhaustsFdTable_ReturnsEmfile);
}
