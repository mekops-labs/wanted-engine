/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <string.h>

#include <wasi.h>
#include <wasi/wasi_types.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * wasi_rights — per-preopen capability masks, seed grants, non-binding lookup
 * ═══════════════════════════════════════════════════════════════════════════ */

TEST_GROUP(wasi_rights);

TEST_SETUP(wasi_rights) {}
TEST_TEAR_DOWN(wasi_rights) {}

/* The read-only grant is the full grant with exactly the write class removed. */
TEST(wasi_rights, ReadonlyClearsTheWriteClassOnly) {
    TEST_ASSERT_EQUAL_UINT64(0, WASI_RIGHTS_READONLY & WASI_RIGHTS_WRITE);
    TEST_ASSERT_EQUAL_UINT64(WASI_RIGHTS_ALL & ~WASI_RIGHTS_WRITE,
                             WASI_RIGHTS_READONLY);

    /* Read-class rights survive. */
    TEST_ASSERT_TRUE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_FD_READ);
    TEST_ASSERT_TRUE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_PATH_OPEN);
    TEST_ASSERT_TRUE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_FD_READDIR);
    TEST_ASSERT_TRUE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_PATH_FILESTAT_GET);

    /* Write-class rights are gone. */
    TEST_ASSERT_FALSE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_FD_WRITE);
    TEST_ASSERT_FALSE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_PATH_CREATE_FILE);
    TEST_ASSERT_FALSE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_PATH_UNLINK_FILE);
    TEST_ASSERT_FALSE(WASI_RIGHTS_READONLY & __WASI_RIGHTS_PATH_REMOVE_DIRECTORY);
}

/* The audited mutate set holds every write-class bit and none of the read or
 * deliberately-excluded (FD_SYNC / FD_FDSTAT_SET_FLAGS) bits. */
TEST(wasi_rights, WriteClassMembership) {
    const uint64_t must_have =
        __WASI_RIGHTS_FD_DATASYNC | __WASI_RIGHTS_FD_WRITE |
        __WASI_RIGHTS_FD_ALLOCATE | __WASI_RIGHTS_PATH_CREATE_DIRECTORY |
        __WASI_RIGHTS_PATH_CREATE_FILE | __WASI_RIGHTS_PATH_LINK_SOURCE |
        __WASI_RIGHTS_PATH_LINK_TARGET | __WASI_RIGHTS_PATH_RENAME_SOURCE |
        __WASI_RIGHTS_PATH_RENAME_TARGET | __WASI_RIGHTS_PATH_FILESTAT_SET_SIZE |
        __WASI_RIGHTS_PATH_FILESTAT_SET_TIMES |
        __WASI_RIGHTS_FD_FILESTAT_SET_SIZE |
        __WASI_RIGHTS_FD_FILESTAT_SET_TIMES | __WASI_RIGHTS_PATH_SYMLINK |
        __WASI_RIGHTS_PATH_REMOVE_DIRECTORY | __WASI_RIGHTS_PATH_UNLINK_FILE;
    TEST_ASSERT_EQUAL_UINT64(must_have, WASI_RIGHTS_WRITE & must_have);

    const uint64_t must_not_have =
        __WASI_RIGHTS_FD_READ | __WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL |
        __WASI_RIGHTS_FD_READDIR | __WASI_RIGHTS_PATH_OPEN |
        __WASI_RIGHTS_FD_SYNC | __WASI_RIGHTS_FD_FDSTAT_SET_FLAGS;
    TEST_ASSERT_EQUAL_UINT64(0, WASI_RIGHTS_WRITE & must_not_have);
}

/* stdio streams keep write/read but not the seek/tell they cannot honour. */
TEST(wasi_rights, StdioDropsSeekTell) {
    TEST_ASSERT_FALSE(WASI_RIGHTS_STDIO &
                      (__WASI_RIGHTS_FD_SEEK | __WASI_RIGHTS_FD_TELL));
    TEST_ASSERT_TRUE(WASI_RIGHTS_STDIO & __WASI_RIGHTS_FD_WRITE);
    TEST_ASSERT_TRUE(WASI_RIGHTS_STDIO & __WASI_RIGHTS_FD_READ);
}

/* A fresh context seeds stdio with the stdio grant and the root image preopen
 * (the lazy fd == -1 entry) with the full grant — the TarFS driver, not a
 * capability mask, enforces the image's read-only nature. */
TEST(wasi_rights, InitContextSeedsStdioAndFullRoot) {
    wasi_ctx_t *ctx = InitWasiContext();
    TEST_ASSERT_NOT_NULL(ctx);

    for (int fd = 0; fd <= 2; fd++) {
        const wasi_preopen_t *p = WasiCtxFindPreopen(ctx, fd);
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_UINT64(WASI_RIGHTS_STDIO, p->rights_base);
        TEST_ASSERT_EQUAL_UINT64(WASI_RIGHTS_STDIO, p->rights_inheriting);
    }

    const wasi_preopen_t *root = NULL;
    for (uint8_t i = 0; i < ctx->preopens_cnt; i++) {
        if (strcmp(ctx->preopens[i].path, "/") == 0)
            root = &ctx->preopens[i];
    }
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, root->fd); /* lazy until first prestat */
    TEST_ASSERT_EQUAL_UINT64(WASI_RIGHTS_ALL, root->rights_base);
    TEST_ASSERT_EQUAL_UINT64(WASI_RIGHTS_ALL, root->rights_inheriting);

    FreeWasiContext(ctx);
}

TEST(wasi_rights, FindPreopenUnknownFdReturnsNull) {
    wasi_ctx_t *ctx = InitWasiContext();
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NULL(WasiCtxFindPreopen(ctx, 99));
    FreeWasiContext(ctx);
}

/* The lookup must not trigger the lazy-bind side effect that resolve_preopen
 * has: probing the not-yet-bound root leaves its entry unbound. */
TEST(wasi_rights, FindPreopenDoesNotBindLazyRoot) {
    wasi_ctx_t *ctx = InitWasiContext();
    TEST_ASSERT_NOT_NULL(ctx);

    /* fd 3 is where the root binds, but nothing has bound it yet. */
    TEST_ASSERT_NULL(WasiCtxFindPreopen(ctx, 3));

    const wasi_preopen_t *root = NULL;
    for (uint8_t i = 0; i < ctx->preopens_cnt; i++) {
        if (strcmp(ctx->preopens[i].path, "/") == 0)
            root = &ctx->preopens[i];
    }
    TEST_ASSERT_NOT_NULL(root);
    TEST_ASSERT_EQUAL_INT(-1, root->fd); /* still lazy — no bind happened */

    FreeWasiContext(ctx);
}

/* The path_open gate: a request is permitted iff it holds no right the parent
 * preopen's inheriting grant withholds. */
TEST(wasi_rights, ReadonlyGrantPermitsReadsRefusesWrites) {
    const uint64_t read_req = __WASI_RIGHTS_FD_READ | __WASI_RIGHTS_FD_SEEK |
                              __WASI_RIGHTS_PATH_OPEN |
                              __WASI_RIGHTS_FD_READDIR |
                              __WASI_RIGHTS_PATH_FILESTAT_GET;
    const uint64_t write_req =
        __WASI_RIGHTS_FD_WRITE | __WASI_RIGHTS_PATH_CREATE_FILE;

    TEST_ASSERT_TRUE(WasiRightsWithin(WASI_RIGHTS_READONLY, read_req));
    TEST_ASSERT_FALSE(WasiRightsWithin(WASI_RIGHTS_READONLY, write_req));

    /* A full grant permits both. */
    TEST_ASSERT_TRUE(WasiRightsWithin(WASI_RIGHTS_ALL, read_req));
    TEST_ASSERT_TRUE(WasiRightsWithin(WASI_RIGHTS_ALL, write_req));
}

TEST(wasi_rights, RightsWithinBoundaries) {
    TEST_ASSERT_TRUE(WasiRightsWithin(WASI_RIGHTS_READONLY, 0));
    TEST_ASSERT_TRUE(
        WasiRightsWithin(WASI_RIGHTS_READONLY, WASI_RIGHTS_READONLY));
    TEST_ASSERT_FALSE(WasiRightsWithin(WASI_RIGHTS_READONLY, WASI_RIGHTS_ALL));
    TEST_ASSERT_FALSE(WasiRightsWithin(0, __WASI_RIGHTS_FD_READ));
}

TEST_GROUP_RUNNER(wasi_rights) {
    RUN_TEST_CASE(wasi_rights, ReadonlyClearsTheWriteClassOnly);
    RUN_TEST_CASE(wasi_rights, WriteClassMembership);
    RUN_TEST_CASE(wasi_rights, StdioDropsSeekTell);
    RUN_TEST_CASE(wasi_rights, InitContextSeedsStdioAndFullRoot);
    RUN_TEST_CASE(wasi_rights, FindPreopenUnknownFdReturnsNull);
    RUN_TEST_CASE(wasi_rights, FindPreopenDoesNotBindLazyRoot);
    RUN_TEST_CASE(wasi_rights, ReadonlyGrantPermitsReadsRefusesWrites);
    RUN_TEST_CASE(wasi_rights, RightsWithinBoundaries);
}
