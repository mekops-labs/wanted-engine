/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include "test-utils.h"

#include <vfs-devfs.h>
#include <vfs.h>

/* This const global lives in vfs-wanted-config.c. */
extern const vfs_driver_t WantedConfigDriver;

/* Call a driver function pointer directly with NULL ctx — valid because
 * WantedConfigDriver does not dereference the ctx argument. */
#define CFG_OPEN(flags)          WantedConfigDriver.Open(NULL, "", (flags))
#define CFG_CLOSE()              WantedConfigDriver.Close(NULL, 0)
#define CFG_READ(buf, n)         WantedConfigDriver.Read(NULL, 0, (buf), (n))
#define CFG_WRITE(buf, n)        WantedConfigDriver.Write(NULL, 0, (buf), (n))
#define CFG_STAT(st)             WantedConfigDriver.Stat(NULL, 0, (st))

/***************************************/
TEST_GROUP(wanted_config_driver);
/***************************************/

TEST_SETUP(wanted_config_driver) {
    WantedConfigDriver.Destroy((vfs_driver_t *)&WantedConfigDriver);
    /* Drain the static cfg_read counter: open, read to EOF, close.
     * cfg_read cycles: 0→n on content, n→0 on EOF. Reading until r==0
     * guarantees cfg_read is 0 regardless of prior state. */
    WantedConfigDriver.Open(NULL, "", 0);
    char drain[128];
    int r;
    do { r = WantedConfigDriver.Read(NULL, 0, drain, sizeof(drain)); } while (r > 0);
    WantedConfigDriver.Close(NULL, 0);
}

TEST_TEAR_DOWN(wanted_config_driver) {
    /* Always close so static state is reset for the next test. */
    WantedConfigDriver.Destroy((vfs_driver_t *)&WantedConfigDriver);
}

TEST(wanted_config_driver, OpenSucceeds) {
    TEST_ASSERT_EQUAL_INT(0, CFG_OPEN(0));
}

TEST(wanted_config_driver, DoubleOpenReturnsBusy) {
    CFG_OPEN(0);
    TEST_ASSERT_EQUAL_INT(-EBUSY, CFG_OPEN(0));
}

TEST(wanted_config_driver, ReadReturnsMetaConfig) {
    CFG_OPEN(0);

    char buf[128] = {0};
    int n = CFG_READ(buf, sizeof(buf));
    TEST_ASSERT_TRUE(n > 0);
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "config_file", 11));
    TEST_ASSERT_TRUE(HasBytes(buf, (size_t)n, "/proc/wapps", 11));
}

TEST(wanted_config_driver, SecondReadReturnsEof) {
    CFG_OPEN(0);

    char buf[128];
    CFG_READ(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, CFG_READ(buf, sizeof(buf)));
}

TEST(wanted_config_driver, ReadNullBufReturnsEinval) {
    CFG_OPEN(0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, CFG_READ(NULL, 16));
}

TEST(wanted_config_driver, ReadBeforeOpenReturnsEbadf) {
    TEST_ASSERT_EQUAL_INT(-EBADF, CFG_READ((char[16]){0}, 16));
}

TEST(wanted_config_driver, WriteReturnsErofs) {
    CFG_OPEN(0);
    TEST_ASSERT_EQUAL_INT(-EROFS, CFG_WRITE("x", 1));
}

TEST(wanted_config_driver, StatReportsCharDeviceAndSize) {
    vfs_stat_t st = {0};
    TEST_ASSERT_EQUAL_INT(0, CFG_STAT(&st));
    TEST_ASSERT_EQUAL_UINT8(VFS_FILETYPE_CHARACTER_DEVICE, st.filetype);
    TEST_ASSERT_TRUE(st.size > 0);
}

TEST(wanted_config_driver, CloseResetsState) {
    CFG_OPEN(0);
    CFG_CLOSE();
    /* After close, a fresh open must succeed. */
    TEST_ASSERT_EQUAL_INT(0, CFG_OPEN(0));
}

TEST_GROUP_RUNNER(wanted_config_driver) {
    RUN_TEST_CASE(wanted_config_driver, OpenSucceeds);
    RUN_TEST_CASE(wanted_config_driver, DoubleOpenReturnsBusy);
    RUN_TEST_CASE(wanted_config_driver, ReadReturnsMetaConfig);
    RUN_TEST_CASE(wanted_config_driver, SecondReadReturnsEof);
    RUN_TEST_CASE(wanted_config_driver, ReadNullBufReturnsEinval);
    RUN_TEST_CASE(wanted_config_driver, ReadBeforeOpenReturnsEbadf);
    RUN_TEST_CASE(wanted_config_driver, WriteReturnsErofs);
    RUN_TEST_CASE(wanted_config_driver, StatReportsCharDeviceAndSize);
    RUN_TEST_CASE(wanted_config_driver, CloseResetsState);
}
