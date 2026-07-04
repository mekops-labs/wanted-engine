/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

/* NIST FIPS 180-4 example digests. */
#define SHA256_EMPTY                                                           \
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define SHA256_ABC                                                             \
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define SHA256_TWO_BLOCK                                                       \
    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
#define TWO_BLOCK_MSG "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"

static vfs_driver_t *drv;

/***************************************/
TEST_GROUP(vfs_sha256);
/***************************************/

TEST_SETUP(vfs_sha256) {
    drv = VfsSha256Init(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
}

TEST_TEAR_DOWN(vfs_sha256) {
    TEST_ASSERT_EQUAL(0, drv->Destroy(drv));
}

static void digestOf(const char *msg, size_t len, char hex[65]) {
    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    if (len > 0)
        TEST_ASSERT_EQUAL((int)len, TRY_DRV(drv, Write, fd, msg, len));
    memset(hex, 0, 65);
    TEST_ASSERT_EQUAL(64, TRY_DRV(drv, Read, fd, hex, 64));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_sha256, EmptyMessage) {
    char hex[65];
    digestOf("", 0, hex);
    TEST_ASSERT_EQUAL_STRING(SHA256_EMPTY, hex);
}

TEST(vfs_sha256, Abc) {
    char hex[65];
    digestOf("abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING(SHA256_ABC, hex);
}

TEST(vfs_sha256, TwoBlockMessage) {
    char hex[65];
    digestOf(TWO_BLOCK_MSG, strlen(TWO_BLOCK_MSG), hex);
    TEST_ASSERT_EQUAL_STRING(SHA256_TWO_BLOCK, hex);
}

TEST(vfs_sha256, ChunkedWritesMatchOneShot) {
    const char *msg = TWO_BLOCK_MSG;
    size_t len = strlen(msg);
    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);

    /* Feed byte by byte across block boundaries. */
    for (size_t i = 0; i < len; i++)
        TEST_ASSERT_EQUAL(1, TRY_DRV(drv, Write, fd, msg + i, 1));

    char hex[65] = {0};
    TEST_ASSERT_EQUAL(64, TRY_DRV(drv, Read, fd, hex, 64));
    TEST_ASSERT_EQUAL_STRING(SHA256_TWO_BLOCK, hex);
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_sha256, PartialReadsDrainDigest) {
    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    TEST_ASSERT_EQUAL(3, TRY_DRV(drv, Write, fd, "abc", 3));

    char hex[65] = {0};
    TEST_ASSERT_EQUAL(16, TRY_DRV(drv, Read, fd, hex, 16));
    TEST_ASSERT_EQUAL(48, TRY_DRV(drv, Read, fd, hex + 16, 64));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Read, fd, hex + 64, 1));
    TEST_ASSERT_EQUAL_STRING_LEN(SHA256_ABC, hex, 64);
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_sha256, WriteAfterReadRejected) {
    int fd = TRY_DRV(drv, Open, "", 0);
    char hex[64];
    TEST_ASSERT_EQUAL(3, TRY_DRV(drv, Write, fd, "abc", 3));
    TEST_ASSERT_EQUAL(64, TRY_DRV(drv, Read, fd, hex, 64));
    TEST_ASSERT_EQUAL(-EINVAL, TRY_DRV(drv, Write, fd, "x", 1));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_sha256, ConcurrentStreamsAreIndependent) {
    int a = TRY_DRV(drv, Open, "", 0);
    int b = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_GREATER_OR_EQUAL(0, a);
    TEST_ASSERT_GREATER_OR_EQUAL(0, b);
    TEST_ASSERT_NOT_EQUAL(a, b);

    TEST_ASSERT_EQUAL(3, TRY_DRV(drv, Write, a, "abc", 3));

    char hex_a[65] = {0};
    char hex_b[65] = {0};
    TEST_ASSERT_EQUAL(64, TRY_DRV(drv, Read, a, hex_a, 64));
    TEST_ASSERT_EQUAL(64, TRY_DRV(drv, Read, b, hex_b, 64));
    TEST_ASSERT_EQUAL_STRING(SHA256_ABC, hex_a);
    TEST_ASSERT_EQUAL_STRING(SHA256_EMPTY, hex_b);

    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, a));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, b));
}

TEST(vfs_sha256, StreamSlotsExhaust) {
    int a = TRY_DRV(drv, Open, "", 0);
    int b = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(-EBUSY, TRY_DRV(drv, Open, "", 0));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, a));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, b));
}

TEST(vfs_sha256, ReopenStartsFresh) {
    char hex[65];
    digestOf("abc", 3, hex);
    digestOf("abc", 3, hex);
    TEST_ASSERT_EQUAL_STRING(SHA256_ABC, hex);
}

TEST(vfs_sha256, SubPathRejected) {
    TEST_ASSERT_EQUAL(-ENOENT, TRY_DRV(drv, Open, "sub", 0));
}

TEST(vfs_sha256, BadFdRejected) {
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Close, 0));
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Write, 7, "x", 1));
    char c;
    TEST_ASSERT_EQUAL(-EBADF, TRY_DRV(drv, Read, -1, &c, 1));
}

TEST_GROUP_RUNNER(vfs_sha256) {
    RUN_TEST_CASE(vfs_sha256, EmptyMessage);
    RUN_TEST_CASE(vfs_sha256, Abc);
    RUN_TEST_CASE(vfs_sha256, TwoBlockMessage);
    RUN_TEST_CASE(vfs_sha256, ChunkedWritesMatchOneShot);
    RUN_TEST_CASE(vfs_sha256, PartialReadsDrainDigest);
    RUN_TEST_CASE(vfs_sha256, WriteAfterReadRejected);
    RUN_TEST_CASE(vfs_sha256, ConcurrentStreamsAreIndependent);
    RUN_TEST_CASE(vfs_sha256, StreamSlotsExhaust);
    RUN_TEST_CASE(vfs_sha256, ReopenStartsFresh);
    RUN_TEST_CASE(vfs_sha256, SubPathRejected);
    RUN_TEST_CASE(vfs_sha256, BadFdRejected);
}
