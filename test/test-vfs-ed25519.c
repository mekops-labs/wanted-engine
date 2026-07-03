/* SPDX-License-Identifier: Apache-2.0 */

/* The dummy platform carries no Ed25519 backend (PlatformEd25519Verify
 * returns -ENOSYS), so these tests cover the device's framing contract and
 * the backend-error propagation; the cryptographic verdict itself is a
 * platform concern exercised on a real target. */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

#define HDR_LEN (PLATFORM_ED25519_KEY_LEN + PLATFORM_ED25519_SIG_LEN)

static vfs_driver_t *drv;

/***************************************/
TEST_GROUP(vfs_ed25519);
/***************************************/

TEST_SETUP(vfs_ed25519) {
    drv = VfsEd25519Init(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
}

TEST_TEAR_DOWN(vfs_ed25519) {
    TEST_ASSERT_EQUAL(0, drv->Destroy(drv));
}

TEST(vfs_ed25519, ReadBeforeFullHeaderRejected) {
    uint8_t hdr[HDR_LEN] = {0};
    char verdict[8];

    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(0, fd);
    TEST_ASSERT_EQUAL(-EINVAL, TRY_DRV(drv, Read, fd, verdict, sizeof(verdict)));

    TEST_ASSERT_EQUAL(HDR_LEN - 1, TRY_DRV(drv, Write, fd, hdr, HDR_LEN - 1));
    TEST_ASSERT_EQUAL(-EINVAL, TRY_DRV(drv, Read, fd, verdict, sizeof(verdict)));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_ed25519, BackendErrorPropagates) {
    uint8_t hdr[HDR_LEN] = {0};
    char verdict[8];

    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(HDR_LEN, TRY_DRV(drv, Write, fd, hdr, HDR_LEN));
    TEST_ASSERT_EQUAL(5, TRY_DRV(drv, Write, fd, "hello", 5));
    /* The dummy platform has no backend: the verdict read reports that. */
    TEST_ASSERT_EQUAL(-ENOSYS, TRY_DRV(drv, Read, fd, verdict, sizeof(verdict)));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_ed25519, SingleStreamPerMount) {
    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(0, fd);
    TEST_ASSERT_EQUAL(-EBUSY, TRY_DRV(drv, Open, "", 0));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Open, "", 0));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, 0));
}

TEST(vfs_ed25519, MessageOverCapRejected) {
    static uint8_t chunk[4096];
    uint8_t hdr[HDR_LEN] = {0};

    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(HDR_LEN, TRY_DRV(drv, Write, fd, hdr, HDR_LEN));
    for (size_t fed = 0; fed < 64 * 1024; fed += sizeof(chunk))
        TEST_ASSERT_EQUAL((int)sizeof(chunk),
                          TRY_DRV(drv, Write, fd, chunk, sizeof(chunk)));
    TEST_ASSERT_EQUAL(-EFBIG, TRY_DRV(drv, Write, fd, chunk, 1));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_ed25519, SubPathRejected) {
    TEST_ASSERT_EQUAL(-ENOENT, TRY_DRV(drv, Open, "sub", 0));
}

TEST(vfs_ed25519, OpsWithoutOpenRejected) {
    char c;
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Write, 0, "x", 1));
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Read, 0, &c, 1));
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Close, 0));
}

TEST_GROUP_RUNNER(vfs_ed25519) {
    RUN_TEST_CASE(vfs_ed25519, ReadBeforeFullHeaderRejected);
    RUN_TEST_CASE(vfs_ed25519, BackendErrorPropagates);
    RUN_TEST_CASE(vfs_ed25519, SingleStreamPerMount);
    RUN_TEST_CASE(vfs_ed25519, MessageOverCapRejected);
    RUN_TEST_CASE(vfs_ed25519, SubPathRejected);
    RUN_TEST_CASE(vfs_ed25519, OpsWithoutOpenRejected);
}
