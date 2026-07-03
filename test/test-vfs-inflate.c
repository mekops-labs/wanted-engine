/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <vfs/vfs-internal.h>

#include "test-vfs-inflate-data.h"

static vfs_driver_t *drv;

/***************************************/
TEST_GROUP(vfs_inflate);
/***************************************/

TEST_SETUP(vfs_inflate) {
    drv = VfsInflateInit(NULL, NULL);
    TEST_ASSERT_NOT_NULL(drv);
}

TEST_TEAR_DOWN(vfs_inflate) {
    TEST_ASSERT_EQUAL(0, drv->Destroy(drv));
}

/* Feed `len` stream bytes in `chunk`-sized writes, draining reads whenever the
 * device pushes back, collecting decompressed output into `out`. */
static size_t pushPull(int fd, const unsigned char *data, size_t len,
                       size_t chunk, unsigned char *out, size_t out_cap) {
    size_t fed = 0;
    size_t got = 0;

    while (fed < len) {
        size_t n = len - fed;
        if (n > chunk)
            n = chunk;
        int w = TRY_DRV(drv, Write, fd, data + fed, n);
        if (w == -EAGAIN)
            w = 0;
        TEST_ASSERT_GREATER_OR_EQUAL(0, w);
        fed += (size_t)w;

        int r;
        do {
            unsigned char buf[512];
            r = TRY_DRV(drv, Read, fd, buf, sizeof(buf));
            if (r > 0) {
                TEST_ASSERT_LESS_OR_EQUAL(out_cap - got, (size_t)r);
                memcpy(out + got, buf, (size_t)r);
                got += (size_t)r;
            }
        } while (r > 0);
        TEST_ASSERT_TRUE(r == 0 || r == -EAGAIN);
    }

    int r;
    do {
        unsigned char buf[512];
        r = TRY_DRV(drv, Read, fd, buf, sizeof(buf));
        if (r > 0) {
            TEST_ASSERT_LESS_OR_EQUAL(out_cap - got, (size_t)r);
            memcpy(out + got, buf, (size_t)r);
            got += (size_t)r;
        }
    } while (r > 0);
    TEST_ASSERT_EQUAL(0, r); /* member complete: EOF, not EAGAIN */

    return got;
}

static void prefixFor(size_t len, unsigned char prefix[4]) {
    prefix[0] = (unsigned char)(len & 0xff);
    prefix[1] = (unsigned char)((len >> 8) & 0xff);
    prefix[2] = (unsigned char)((len >> 16) & 0xff);
    prefix[3] = (unsigned char)((len >> 24) & 0xff);
}

TEST(vfs_inflate, SmallMemberRoundTrip) {
    static unsigned char out[SMALL_PLAIN_LEN + 16];
    unsigned char prefix[4];

    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(0, fd);

    prefixFor(sizeof(small_gz), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));
    size_t got =
        pushPull(fd, small_gz, sizeof(small_gz), sizeof(small_gz), out,
                 sizeof(out));

    TEST_ASSERT_EQUAL(SMALL_PLAIN_LEN, got);
    TEST_ASSERT_EQUAL_MEMORY(small_plain, out, SMALL_PLAIN_LEN);
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, TinyChunksRoundTrip) {
    static unsigned char out[SMALL_PLAIN_LEN + 16];
    unsigned char prefix[4];

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(small_gz), prefix);
    /* Prefix split across two writes, member in 3-byte chunks. */
    TEST_ASSERT_EQUAL(1, TRY_DRV(drv, Write, fd, prefix, 1));
    TEST_ASSERT_EQUAL(3, TRY_DRV(drv, Write, fd, prefix + 1, 3));
    size_t got = pushPull(fd, small_gz, sizeof(small_gz), 3, out, sizeof(out));

    TEST_ASSERT_EQUAL(SMALL_PLAIN_LEN, got);
    TEST_ASSERT_EQUAL_MEMORY(small_plain, out, SMALL_PLAIN_LEN);
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, LargeOutputRecyclesBuffer) {
    /* 20000 B of output through the 4 KiB output buffer forces several
     * drain/refill cycles and exercises the history window. */
    static unsigned char out[BIG_PLAIN_LEN + 16];
    unsigned char prefix[4];

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(big_gz), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));
    size_t got = pushPull(fd, big_gz, sizeof(big_gz), 256, out, sizeof(out));

    TEST_ASSERT_EQUAL(BIG_PLAIN_LEN, got);
    for (size_t i = 0; i < BIG_PLAIN_LEN; i++) {
        unsigned char want = (unsigned char)((i * 7 + (i >> 8) * 13) & 0xff);
        if (out[i] != want) {
            TEST_ASSERT_EQUAL_UINT8(want, out[i]);
        }
    }
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, CorruptCrcFails) {
    static unsigned char member[sizeof(small_gz)];
    static unsigned char out[SMALL_PLAIN_LEN + 16];
    unsigned char prefix[4];

    memcpy(member, small_gz, sizeof(member));
    member[sizeof(member) - 5] ^= 0xff; /* flip a CRC byte in the trailer */

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(member), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));

    size_t fed = 0;
    int last = 0;
    while (fed < sizeof(member)) {
        last = TRY_DRV(drv, Write, fd, member + fed, sizeof(member) - fed);
        if (last < 0)
            break;
        fed += (size_t)last;
        int r;
        do {
            r = TRY_DRV(drv, Read, fd, out, sizeof(out));
        } while (r > 0);
        if (r == -EIO) {
            last = r;
            break;
        }
    }
    if (last >= 0) {
        int r;
        do {
            r = TRY_DRV(drv, Read, fd, out, sizeof(out));
        } while (r > 0);
        last = r;
    }
    TEST_ASSERT_EQUAL(-EIO, last);
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, BadMagicFails) {
    static unsigned char member[sizeof(small_gz)];
    unsigned char prefix[4];
    unsigned char buf[8];

    memcpy(member, small_gz, sizeof(member));
    member[0] = 0x42;

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(member), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));
    TRY_DRV(drv, Write, fd, member, sizeof(member));
    TEST_ASSERT_EQUAL(-EIO, TRY_DRV(drv, Read, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, WriteBeyondDeclaredRejected) {
    static unsigned char out[SMALL_PLAIN_LEN + 16];
    unsigned char prefix[4];
    unsigned char extra = 0x00;

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(small_gz), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));
    (void)pushPull(fd, small_gz, sizeof(small_gz), sizeof(small_gz), out,
                   sizeof(out));
    TEST_ASSERT_EQUAL(-EFBIG, TRY_DRV(drv, Write, fd, &extra, 1));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, ReadMidStreamIsEagain) {
    unsigned char prefix[4];
    unsigned char buf[8];

    int fd = TRY_DRV(drv, Open, "", 0);
    prefixFor(sizeof(small_gz), prefix);
    TEST_ASSERT_EQUAL(4, TRY_DRV(drv, Write, fd, prefix, 4));
    TEST_ASSERT_EQUAL(10, TRY_DRV(drv, Write, fd, small_gz, 10));
    TEST_ASSERT_EQUAL(-EAGAIN, TRY_DRV(drv, Read, fd, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, DeclaredTooSmallRejected) {
    unsigned char prefix[4] = {17, 0, 0, 0}; /* below header+trailer floor */

    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(-EINVAL, TRY_DRV(drv, Write, fd, prefix, 4));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
}

TEST(vfs_inflate, SingleStreamPerMount) {
    int fd = TRY_DRV(drv, Open, "", 0);
    TEST_ASSERT_EQUAL(0, fd);
    TEST_ASSERT_EQUAL(-EBUSY, TRY_DRV(drv, Open, "", 0));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, fd));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Open, "", 0));
    TEST_ASSERT_EQUAL(0, TRY_DRV(drv, Close, 0));
}

TEST(vfs_inflate, SubPathRejected) {
    TEST_ASSERT_EQUAL(-ENOENT, TRY_DRV(drv, Open, "sub", 0));
}

TEST_GROUP_RUNNER(vfs_inflate) {
    RUN_TEST_CASE(vfs_inflate, SmallMemberRoundTrip);
    RUN_TEST_CASE(vfs_inflate, TinyChunksRoundTrip);
    RUN_TEST_CASE(vfs_inflate, LargeOutputRecyclesBuffer);
    RUN_TEST_CASE(vfs_inflate, CorruptCrcFails);
    RUN_TEST_CASE(vfs_inflate, BadMagicFails);
    RUN_TEST_CASE(vfs_inflate, WriteBeyondDeclaredRejected);
    RUN_TEST_CASE(vfs_inflate, ReadMidStreamIsEagain);
    RUN_TEST_CASE(vfs_inflate, DeclaredTooSmallRejected);
    RUN_TEST_CASE(vfs_inflate, SingleStreamPerMount);
    RUN_TEST_CASE(vfs_inflate, SubPathRejected);
}
