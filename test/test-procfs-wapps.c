/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include "dummy-fs.h"
#include <vfs-procfs.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

/* Exercises the /proc/wapps/<name>/<leaf> observability surface
 * (vfs-procfs-wapps.c) against seeded dummy platform state. */

static vfs_ctx_t vfs;

static wapp_state_t mkstate(const char *name) {
    wapp_state_t s;
    memset(&s, 0, sizeof(s));
    strncpy(s.name, name, WAPP_MAX_NAME_LEN - 1);
    strncpy(s.image, "img", WAPP_MAX_NAME_LEN - 1);
    strncpy(s.version, "1.2.3", WAPP_MAX_VERSION_LEN - 1);
    s.id = 2;
    s.status = RUNNING;
    s.exit_code = WAPP_EXIT_CODE_NONE;
    s.mem_pages_cur = 1;
    s.mem_pages_max = 2;
    s.mem_bytes_cur = 65536;
    s.mem_bytes_max = 131072;
    return s;
}

TEST_GROUP(procfs_wapps);

TEST_SETUP(procfs_wapps) {
    vfs = VfsInit();
    VfsSetPrivileged(vfs, true);
    ProcFs_RegisterDir(vfs, "wapps", &WappsProcDirOps, true);
    DummyWappStateReset();
}

TEST_TEAR_DOWN(procfs_wapps) {
    DummyWappStateReset();
    VfsDestroy(&vfs);
}

TEST(procfs_wapps, EnumeratesRunningWapps) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);

    int fd = VfsOpen(vfs, "/proc/wapps", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    uint8_t buf[256];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(0,
                          VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "w1", 2));
    VfsClose(vfs, fd);
}

TEST(procfs_wapps, WappDirListsLeaves) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);

    int fd = VfsOpen(vfs, "/proc/wapps/w1", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[256];
    size_t used = 0;
    uint64_t cookie = 0;
    TEST_ASSERT_EQUAL_INT(0,
                          VfsReadDir(vfs, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "state", 5));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "memory", 6));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "exit_code", 9));
    VfsClose(vfs, fd);
}

TEST(procfs_wapps, StateLeafReportsRunning) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);

    char buf[64] = {0};
    int fd = VfsOpen(vfs, "/proc/wapps/w1/state", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    int n = VfsRead(vfs, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_STRING("running", buf);
    VfsClose(vfs, fd);
}

TEST(procfs_wapps, MemoryLeafRendersAccounting) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);

    char buf[128] = {0};
    int fd = VfsOpen(vfs, "/proc/wapps/w1/memory", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    int n = VfsRead(vfs, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "linear_cur:\t65536 B"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "linear_max:\t131072 B"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "pages_cur:\t1"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "pages_max:\t2"));
    VfsClose(vfs, fd);
}

TEST(procfs_wapps, UnknownWappReturnsEnoent) {
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsOpen(vfs, "/proc/wapps/ghost/state", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          VfsOpen(vfs, "/proc/wapps/ghost", VFS_O_RDONLY));
}

TEST(procfs_wapps, UnknownLeafReturnsEnoent) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, VfsOpen(vfs, "/proc/wapps/w1/bogus", VFS_O_RDONLY));
}

TEST(procfs_wapps, UnprivilegedCannotOpen) {
    wapp_state_t s = mkstate("w1");
    DummyWappStateSeed(&s, 1);
    VfsSetPrivileged(vfs, false);
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          VfsOpen(vfs, "/proc/wapps/w1/state", VFS_O_RDONLY));
}

TEST_GROUP_RUNNER(procfs_wapps) {
    RUN_TEST_CASE(procfs_wapps, EnumeratesRunningWapps);
    RUN_TEST_CASE(procfs_wapps, WappDirListsLeaves);
    RUN_TEST_CASE(procfs_wapps, StateLeafReportsRunning);
    RUN_TEST_CASE(procfs_wapps, MemoryLeafRendersAccounting);
    RUN_TEST_CASE(procfs_wapps, UnknownWappReturnsEnoent);
    RUN_TEST_CASE(procfs_wapps, UnknownLeafReturnsEnoent);
    RUN_TEST_CASE(procfs_wapps, UnprivilegedCannotOpen);
}
