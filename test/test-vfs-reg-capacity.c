/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <registry-mem.h>
#include <registry.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* vfs_reg_capacity — the read-only "capacity" node of the registry tree. */

extern const vfs_driver_t WantedRegistryDriver;

TEST_GROUP(vfs_reg_capacity);

static const vfs_driver_t *drv;

TEST_SETUP(vfs_reg_capacity) {
    DummyRegistryReset();
    drv = &WantedRegistryDriver;
    drv->Destroy((vfs_driver_t *)drv);
}

TEST_TEAR_DOWN(vfs_reg_capacity) {
    drv->Destroy((vfs_driver_t *)drv);
    DummyRegistryReset();
}

TEST(vfs_reg_capacity, Read_ReturnsOneObjectForEachStore) {
    char buf[640];
    int n;

    RegistryMemSetLimit(100000);
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    n = drv->Read(drv->ctx, 0, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_EQUAL_CHAR('[', buf[0]);
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"name\":\"mem\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"kind\":\"mem\""));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"total\":100000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free\":100000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mappable\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"removable\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"present\":true"));
    TEST_ASSERT_EQUAL_CHAR('\n', buf[n - 1]);
}

TEST(vfs_reg_capacity, Read_FollowsWhatAnInstallUsed) {
    char buf[640];
    int n;

    RegistryMemSetLimit(100000);
    TEST_ASSERT_EQUAL_INT(
        5, RegistryWrite(START_WRITE, "app:1", (const uint8_t *)"hello", 5));
    TEST_ASSERT_EQUAL_INT(0, RegistryWrite(FINISH_WRITE, NULL, NULL, 0));

    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    n = drv->Read(drv->ctx, 0, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"free\":99995"));
}

TEST(vfs_reg_capacity, Read_InShortPieces_ReturnsTheWholeBody) {
    char whole[640];
    char piece[16];
    size_t at = 0;
    int n;

    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    while ((n = drv->Read(drv->ctx, 0, piece, sizeof(piece))) > 0) {
        memcpy(whole + at, piece, (size_t)n);
        at += (size_t)n;
    }
    TEST_ASSERT_EQUAL_INT(0, n);
    TEST_ASSERT_TRUE(at > 20);
    TEST_ASSERT_EQUAL_CHAR(']', whole[at - 2]);
}

TEST(vfs_reg_capacity, Open_ForWrite_ReturnsErofs) {
    TEST_ASSERT_EQUAL_INT(-EROFS,
                          drv->Open(drv->ctx, "capacity", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Open(drv->ctx, "capacity", VFS_O_RDWR));
}

TEST(vfs_reg_capacity, SecondOpen_ReturnsEbusyUntilTheFirstCloses) {
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EBUSY,
                          drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, 0));
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
}

TEST(vfs_reg_capacity, Read_WithoutAnOpen_ReturnsEbadf) {
    char buf[8];

    TEST_ASSERT_EQUAL_INT(-EBADF, drv->Read(drv->ctx, 0, buf, sizeof(buf)));
}

TEST(vfs_reg_capacity, Write_ReturnsErofs) {
    TEST_ASSERT_EQUAL_INT(0, drv->Open(drv->ctx, "capacity", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Write(drv->ctx, 0, "x", 1));
}

TEST(vfs_reg_capacity, Render_WithASmallBuffer_ReturnsEmsgsize) {
    char tiny[8];

    TEST_ASSERT_EQUAL_INT(-EMSGSIZE,
                          WantedRenderRegistryCapacity(tiny, sizeof(tiny)));
}

TEST_GROUP_RUNNER(vfs_reg_capacity) {
    RUN_TEST_CASE(vfs_reg_capacity, Read_ReturnsOneObjectForEachStore);
    RUN_TEST_CASE(vfs_reg_capacity, Read_FollowsWhatAnInstallUsed);
    RUN_TEST_CASE(vfs_reg_capacity, Read_InShortPieces_ReturnsTheWholeBody);
    RUN_TEST_CASE(vfs_reg_capacity, Open_ForWrite_ReturnsErofs);
    RUN_TEST_CASE(vfs_reg_capacity, SecondOpen_ReturnsEbusyUntilTheFirstCloses);
    RUN_TEST_CASE(vfs_reg_capacity, Read_WithoutAnOpen_ReturnsEbadf);
    RUN_TEST_CASE(vfs_reg_capacity, Write_ReturnsErofs);
    RUN_TEST_CASE(vfs_reg_capacity, Render_WithASmallBuffer_ReturnsEmsgsize);
}
