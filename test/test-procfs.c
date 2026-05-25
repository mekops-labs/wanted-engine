#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <vfs-procfs.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>
#include <wanted-vfs-api.h>

static vfs_ctx_t vfs;

static int _ReadInfo(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    const char data[] = "info-data";
    size_t n = sizeof(data) - 1;
    if (n > bufLen) n = bufLen;
    memcpy(buf, data, n);
    return (int)n;
}

static int _ReadPriv(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    const char data[] = "secret";
    size_t n = sizeof(data) - 1;
    if (n > bufLen) n = bufLen;
    memcpy(buf, data, n);
    return (int)n;
}

/***************************************/
TEST_GROUP(procfs_register);
/***************************************/

TEST_SETUP(procfs_register) { vfs = VfsInit(); }

TEST_TEAR_DOWN(procfs_register) { VfsDestroy(&vfs); }

TEST(procfs_register, RejectsInvalidArgs) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, ProcFs_Register(NULL,  "x", _ReadInfo, false));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ProcFs_Register(vfs,   NULL, _ReadInfo, false));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ProcFs_Register(vfs,   "",  _ReadInfo, false));
    TEST_ASSERT_EQUAL_INT(-EINVAL, ProcFs_Register(vfs,   "x", NULL,      false));
}

TEST(procfs_register, RejectsNameTooLong) {
    char long_name[MAX_ENTRY_NAME_LEN + 2];
    memset(long_name, 'a', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG, ProcFs_Register(vfs, long_name, _ReadInfo, false));
}

TEST(procfs_register, RejectsDuplicate) {
    TEST_ASSERT_EQUAL_INT(0, ProcFs_Register(vfs, "info", _ReadInfo, false));
    TEST_ASSERT_EQUAL_INT(-EEXIST, ProcFs_Register(vfs, "info", _ReadInfo, false));
}

TEST(procfs_register, RejectsTableFull) {
    int r = 0;
    char name[4] = "a00";
    for (int i = 0; i <= VFS_PROCFS_MAX_ENTRIES; i++) {
        name[1] = '0' + i / 10;
        name[2] = '0' + i % 10;
        r = ProcFs_Register(vfs, name, _ReadInfo, false);
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, r);
}

TEST_GROUP_RUNNER(procfs_register) {
    RUN_TEST_CASE(procfs_register, RejectsInvalidArgs);
    RUN_TEST_CASE(procfs_register, RejectsNameTooLong);
    RUN_TEST_CASE(procfs_register, RejectsDuplicate);
    RUN_TEST_CASE(procfs_register, RejectsTableFull);
}

/***************************************/
TEST_GROUP(procfs_open);
/***************************************/

TEST_SETUP(procfs_open) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "info", _ReadInfo, false);
    ProcFs_Register(vfs, "secret", _ReadPriv, true);
}

TEST_TEAR_DOWN(procfs_open) { VfsDestroy(&vfs); }

TEST(procfs_open, OpenRootSucceeds) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, NULL, VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, out_err);
    ProcFs_Close(vfs, h);

    h = ProcFs_Open(vfs, "", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);
    ProcFs_Close(vfs, h);
}

TEST(procfs_open, OpenKnownEntry) {
    int out_err = -1;
    void *h = ProcFs_Open(vfs, "info", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, out_err);
    ProcFs_Close(vfs, h);
}

TEST(procfs_open, OpenUnknownEntryReturnsEnoent) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, "no-such-entry", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NULL(h);
    TEST_ASSERT_EQUAL_INT(-ENOENT, out_err);
}

TEST(procfs_open, PrivilegedEntryBlockedWithoutPrivilege) {
    vfs->privileged = false;
    int out_err = 0;
    void *h = ProcFs_Open(vfs, "secret", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NULL(h);
    TEST_ASSERT_EQUAL_INT(-EACCES, out_err);
}

TEST(procfs_open, PrivilegedEntryAllowedWithPrivilege) {
    VfsSetPrivileged(vfs, true);
    int out_err = -1;
    void *h = ProcFs_Open(vfs, "secret", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_INT(0, out_err);
    ProcFs_Close(vfs, h);
}

TEST_GROUP_RUNNER(procfs_open) {
    RUN_TEST_CASE(procfs_open, OpenRootSucceeds);
    RUN_TEST_CASE(procfs_open, OpenKnownEntry);
    RUN_TEST_CASE(procfs_open, OpenUnknownEntryReturnsEnoent);
    RUN_TEST_CASE(procfs_open, PrivilegedEntryBlockedWithoutPrivilege);
    RUN_TEST_CASE(procfs_open, PrivilegedEntryAllowedWithPrivilege);
}

/***************************************/
TEST_GROUP(procfs_read);
/***************************************/

TEST_SETUP(procfs_read) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "info", _ReadInfo, false);
}

TEST_TEAR_DOWN(procfs_read) { VfsDestroy(&vfs); }

TEST(procfs_read, ReturnsDataOnFirstRead) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, "info", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    char buf[32] = {0};
    int n = ProcFs_Read(vfs, h, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("info-data", buf);

    /* Second read: EOF. */
    n = ProcFs_Read(vfs, h, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, n);

    ProcFs_Close(vfs, h);
}

TEST(procfs_read, RootHandleReturnsEbadf) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, NULL, VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    char buf[8];
    int n = ProcFs_Read(vfs, h, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-EBADF, n);

    ProcFs_Close(vfs, h);
}

TEST_GROUP_RUNNER(procfs_read) {
    RUN_TEST_CASE(procfs_read, ReturnsDataOnFirstRead);
    RUN_TEST_CASE(procfs_read, RootHandleReturnsEbadf);
}

/***************************************/
TEST_GROUP(procfs_stat);
/***************************************/

TEST_SETUP(procfs_stat) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "info", _ReadInfo, false);
}

TEST_TEAR_DOWN(procfs_stat) { VfsDestroy(&vfs); }

TEST(procfs_stat, RootIsDirectory) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, NULL, VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, ProcFs_Stat(vfs, h, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_DIRECTORY, st.filetype);

    ProcFs_Close(vfs, h);
}

TEST(procfs_stat, EntryIsRegularFile) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, "info", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, ProcFs_Stat(vfs, h, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);

    ProcFs_Close(vfs, h);
}

TEST_GROUP_RUNNER(procfs_stat) {
    RUN_TEST_CASE(procfs_stat, RootIsDirectory);
    RUN_TEST_CASE(procfs_stat, EntryIsRegularFile);
}

/***************************************/
TEST_GROUP(procfs_readdir);
/***************************************/

TEST_SETUP(procfs_readdir) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "info",   _ReadInfo, false);
    ProcFs_Register(vfs, "status", _ReadInfo, false);
    ProcFs_Register(vfs, "secret", _ReadPriv, true);
}

TEST_TEAR_DOWN(procfs_readdir) { VfsDestroy(&vfs); }

TEST(procfs_readdir, UnprivilegedListOmitsPrivileged) {
    vfs->privileged = false;
    int out_err = 0;
    void *h = ProcFs_Open(vfs, NULL, VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, ProcFs_ReadDir(vfs, h, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_TRUE(used > 0);

    /* Only "info" and "status" should appear — "secret" is privileged. */
    TEST_ASSERT_FALSE(HasBytes(buf, used, "secret", 6));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "info",   4));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "status", 6));

    ProcFs_Close(vfs, h);
}

TEST(procfs_readdir, PrivilegedListIncludesAll) {
    VfsSetPrivileged(vfs, true);
    int out_err = 0;
    void *h = ProcFs_Open(vfs, NULL, VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(0, ProcFs_ReadDir(vfs, h, buf, sizeof(buf), &cookie, &used));

    TEST_ASSERT_TRUE(HasBytes(buf, used, "secret", 6));
    TEST_ASSERT_TRUE(HasBytes(buf, used, "info",   4));

    ProcFs_Close(vfs, h);
}

TEST(procfs_readdir, NonRootHandleReturnsEbadf) {
    int out_err = 0;
    void *h = ProcFs_Open(vfs, "info", VFS_O_RDONLY, &out_err);
    TEST_ASSERT_NOT_NULL(h);

    uint8_t buf[64];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(-EBADF, ProcFs_ReadDir(vfs, h, buf, sizeof(buf), &cookie, &used));

    ProcFs_Close(vfs, h);
}

TEST_GROUP_RUNNER(procfs_readdir) {
    RUN_TEST_CASE(procfs_readdir, UnprivilegedListOmitsPrivileged);
    RUN_TEST_CASE(procfs_readdir, PrivilegedListIncludesAll);
    RUN_TEST_CASE(procfs_readdir, NonRootHandleReturnsEbadf);
}

/***************************************/
TEST_GROUP(procfs_via_vfs);
/***************************************/

TEST_SETUP(procfs_via_vfs) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "info",   _ReadInfo, false);
    ProcFs_Register(vfs, "secret", _ReadPriv, true);
}

TEST_TEAR_DOWN(procfs_via_vfs) { VfsDestroy(&vfs); }

TEST(procfs_via_vfs, OpenAndReadEntry) {
    int fd = VfsOpen(vfs, "/proc/info", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[32] = {0};
    int n = VfsRead(vfs, fd, buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_EQUAL_STRING("info-data", buf);

    TEST_ASSERT_EQUAL_INT(0, VfsClose(vfs, fd));
}

TEST(procfs_via_vfs, StatEntryIsRegularFile) {
    int fd = VfsOpen(vfs, "/proc/info", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, VfsStat(vfs, fd, &st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_REGULAR_FILE, st.filetype);

    VfsClose(vfs, fd);
}

TEST(procfs_via_vfs, UnprivilegedCannotOpenSecretEntry) {
    vfs->privileged = false;
    int fd = VfsOpen(vfs, "/proc/secret", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EACCES, fd);
}

TEST(procfs_via_vfs, PrivilegedCanOpenSecretEntry) {
    VfsSetPrivileged(vfs, true);
    int fd = VfsOpen(vfs, "/proc/secret", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    VfsClose(vfs, fd);
}

TEST(procfs_via_vfs, MissingEntryReturnsEnoent) {
    int fd = VfsOpen(vfs, "/proc/no-such", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST_GROUP_RUNNER(procfs_via_vfs) {
    RUN_TEST_CASE(procfs_via_vfs, OpenAndReadEntry);
    RUN_TEST_CASE(procfs_via_vfs, StatEntryIsRegularFile);
    RUN_TEST_CASE(procfs_via_vfs, UnprivilegedCannotOpenSecretEntry);
    RUN_TEST_CASE(procfs_via_vfs, PrivilegedCanOpenSecretEntry);
    RUN_TEST_CASE(procfs_via_vfs, MissingEntryReturnsEnoent);
}

/***************************************/
TEST_GROUP(procfs_clock_quality);
/***************************************/

TEST_SETUP(procfs_clock_quality) {
    vfs = VfsInit();
    ProcFs_Register(vfs, "clock_quality",
                    WantedProcReadClockQuality, false);
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
}

TEST_TEAR_DOWN(procfs_clock_quality) {
    VfsDestroy(&vfs);
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
}

TEST(procfs_clock_quality, DefaultIsUncalibrated) {
    int fd = VfsOpen(vfs, "/proc/clock_quality", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t b = 0xFF;
    int n = VfsRead(vfs, fd, &b, 1);
    TEST_ASSERT_EQUAL_INT(1, n);
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_UNCALIBRATED, b);
    TEST_ASSERT_TRUE(b <= 3);

    VfsClose(vfs, fd);
}

TEST(procfs_clock_quality, ReflectsCalibrationUpdate) {
    WantedSetClockQuality(WANTED_CLOCK_SNTP_CALIBRATED);
    int fd = VfsOpen(vfs, "/proc/clock_quality", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    uint8_t b = 0xFF;
    TEST_ASSERT_EQUAL_INT(1, VfsRead(vfs, fd, &b, 1));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SNTP_CALIBRATED, b);
    VfsClose(vfs, fd);

    /* Re-open and re-read to observe the new value. */
    WantedSetClockQuality(WANTED_CLOCK_SIMPLE_CALIBRATION);
    fd = VfsOpen(vfs, "/proc/clock_quality", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    b = 0xFF;
    TEST_ASSERT_EQUAL_INT(1, VfsRead(vfs, fd, &b, 1));
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_SIMPLE_CALIBRATION, b);
    VfsClose(vfs, fd);
}

TEST(procfs_clock_quality, RejectsOutOfRangeWrite) {
    WantedSetClockQuality(WANTED_CLOCK_HARDWARE_RTC);
    WantedSetClockQuality(99); /* must be ignored */
    TEST_ASSERT_EQUAL_UINT8(WANTED_CLOCK_HARDWARE_RTC,
                            WantedGetClockQuality());
}

TEST_GROUP_RUNNER(procfs_clock_quality) {
    RUN_TEST_CASE(procfs_clock_quality, DefaultIsUncalibrated);
    RUN_TEST_CASE(procfs_clock_quality, ReflectsCalibrationUpdate);
    RUN_TEST_CASE(procfs_clock_quality, RejectsOutOfRangeWrite);
}
