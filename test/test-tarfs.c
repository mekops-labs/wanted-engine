#include "unity_fixture.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <vfs-tarfs.h>
#include <vfs.h>

/* Minimal TAR header builder for in-memory fixture archives.
 * Fills only the fields our parser reads: name, size (11-digit octal NUL),
 * typeflag, and the ustar magic so the block doesn't look empty. */
static void TarHeader(uint8_t hdr[512], const char *name, uint32_t size,
                      char typeflag) {
    memset(hdr, 0, 512);
    strncpy((char *)hdr, name, 99);
    for (int i = 10; i >= 0; i--) {
        hdr[124 + i] = (uint8_t)('0' + (size & 7));
        size >>= 3;
    }
    hdr[124 + 11] = '\0';
    hdr[156] = (uint8_t)typeflag;
    memcpy(hdr + 257, "ustar", 5);
}

static uint8_t singleLayer[512 * 4]; /* header + data + 2 zero blocks */

/***************************************/
TEST_GROUP(tarfs_phase2);
/***************************************/

TEST_SETUP(tarfs_phase2) { memset(singleLayer, 0, sizeof(singleLayer)); }

TEST_TEAR_DOWN(tarfs_phase2) {}

TEST(tarfs_phase2, RejectsInvalidArgs) {
    uint8_t *layers[1] = {singleLayer};
    size_t lens[1] = {sizeof(singleLayer)};

    TEST_ASSERT_NULL(TarFsInit(NULL, lens, 1));
    TEST_ASSERT_NULL(TarFsInit(layers, NULL, 1));
    TEST_ASSERT_NULL(TarFsInit(layers, lens, 0));
    TEST_ASSERT_NULL(TarFsInit(layers, lens, TARFS_MAX_LAYERS + 1));
}

TEST(tarfs_phase2, IndexesSingleFileAndPrefetchesWasm) {
    const char payload[] = "hello";
    TarHeader(singleLayer, "app.wasm", sizeof(payload) - 1, '0');
    memcpy(singleLayer + 512, payload, sizeof(payload) - 1);

    uint8_t *layers[1] = {singleLayer};
    size_t lens[1] = {sizeof(singleLayer)};

    vfs_tarfs_ctx_t *ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(ctx));

    size_t wasm_len = 0;
    const uint8_t *wasm = TarFsEntrypointWasm(ctx, &wasm_len);
    TEST_ASSERT_NOT_NULL(wasm);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload) - 1, wasm_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, wasm, sizeof(payload) - 1);

    size_t man_len = 42;
    const uint8_t *manifest = TarFsEntrypointManifest(ctx, &man_len);
    TEST_ASSERT_NULL(manifest);
    TEST_ASSERT_EQUAL_UINT32(0, man_len);

    TarFsDestroy(ctx);
}

TEST(tarfs_phase2, NewerLayerShadowsOlder) {
    static uint8_t baseLayer[512 * 4];
    static uint8_t topLayer[512 * 4];

    memset(baseLayer, 0, sizeof(baseLayer));
    memset(topLayer, 0, sizeof(topLayer));

    TarHeader(baseLayer, "app.wasm", 4, '0');
    memcpy(baseLayer + 512, "OLD!", 4);
    TarHeader(topLayer, "app.wasm", 4, '0');
    memcpy(topLayer + 512, "NEW!", 4);

    /* index 0 is newest */
    uint8_t *layers[2] = {topLayer, baseLayer};
    size_t lens[2] = {sizeof(topLayer), sizeof(baseLayer)};

    vfs_tarfs_ctx_t *ctx = TarFsInit(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(ctx));

    size_t n = 0;
    const uint8_t *wasm = TarFsEntrypointWasm(ctx, &n);
    TEST_ASSERT_EQUAL_UINT32(4, n);
    TEST_ASSERT_EQUAL_MEMORY("NEW!", wasm, 4);

    TarFsDestroy(ctx);
}

TEST(tarfs_phase2, WhiteoutSuppressesEntrypointPrefetch) {
    static uint8_t baseLayer[512 * 4];
    static uint8_t topLayer[512 * 4];

    memset(baseLayer, 0, sizeof(baseLayer));
    memset(topLayer, 0, sizeof(topLayer));

    TarHeader(baseLayer, "app.wasm", 4, '0');
    memcpy(baseLayer + 512, "OLD!", 4);
    /* whiteout for app.wasm in the top layer */
    TarHeader(topLayer, ".wh.app.wasm", 0, '0');

    uint8_t *layers[2] = {topLayer, baseLayer};
    size_t lens[2] = {sizeof(topLayer), sizeof(baseLayer)};

    vfs_tarfs_ctx_t *ctx = TarFsInit(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(ctx);
    /* Whiteout entry recorded; shadowed app.wasm from base layer not added */
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(ctx));

    size_t n = 123;
    const uint8_t *wasm = TarFsEntrypointWasm(ctx, &n);
    TEST_ASSERT_NULL(wasm);
    TEST_ASSERT_EQUAL_UINT32(0, n);

    TarFsDestroy(ctx);
}

TEST_GROUP_RUNNER(tarfs_phase2) {
    RUN_TEST_CASE(tarfs_phase2, RejectsInvalidArgs);
    RUN_TEST_CASE(tarfs_phase2, IndexesSingleFileAndPrefetchesWasm);
    RUN_TEST_CASE(tarfs_phase2, NewerLayerShadowsOlder);
    RUN_TEST_CASE(tarfs_phase2, WhiteoutSuppressesEntrypointPrefetch);
}

/***************************************/
TEST_GROUP(tarfs_phase5);
/***************************************/

/* Reused across the file/dir op tests. Two pre-built fixtures cover the
 * common shapes: a single regular file, and a layered archive with several
 * files plus an implicit subdirectory. */
static uint8_t p5_singleLayer[512 * 4];
static uint8_t p5_baseLayer[512 * 8];
static uint8_t p5_topLayer[512 * 4];
static vfs_tarfs_ctx_t *p5_ctx;

TEST_SETUP(tarfs_phase5) {
    memset(p5_singleLayer, 0, sizeof(p5_singleLayer));
    memset(p5_baseLayer, 0, sizeof(p5_baseLayer));
    memset(p5_topLayer, 0, sizeof(p5_topLayer));
    p5_ctx = NULL;
}

TEST_TEAR_DOWN(tarfs_phase5) {
    if (p5_ctx) {
        TarFsDestroy(p5_ctx);
        p5_ctx = NULL;
    }
}

static vfs_tarfs_ctx_t *BuildSingleFileCtx(const char *name,
                                           const char *payload, size_t plen) {
    TarHeader(p5_singleLayer, name, (uint32_t)plen, '0');
    memcpy(p5_singleLayer + 512, payload, plen);
    uint8_t *layers[1] = {p5_singleLayer};
    size_t lens[1] = {sizeof(p5_singleLayer)};
    return TarFsInit(layers, lens, 1);
}

TEST(tarfs_phase5, OpenReadsRegularFileAndStops) {
    const char payload[] = "HELLO_TARFS";
    p5_ctx = BuildSingleFileCtx("app.wasm", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    void *h = TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    char buf[64] = {0};
    int n = TarFs_Read(p5_ctx, h, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(sizeof(payload) - 1, n);
    TEST_ASSERT_EQUAL_STRING(payload, buf);

    /* Subsequent read past EOF returns 0. */
    n = TarFs_Read(p5_ctx, h, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(0, n);

    TEST_ASSERT_EQUAL_INT(0, TarFs_Close(p5_ctx, h));
}

TEST(tarfs_phase5, OpenRejectsWriteFlags) {
    const char payload[] = "ro";
    p5_ctx = BuildSingleFileCtx("app.wasm", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    TEST_ASSERT_NULL(TarFs_Open(p5_ctx, "/app.wasm", VFS_O_WRONLY));
    TEST_ASSERT_NULL(TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDWR));
    TEST_ASSERT_NULL(
        TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDONLY | VFS_O_CREAT));
    TEST_ASSERT_NULL(
        TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDONLY | VFS_O_TRUNC));
}

TEST(tarfs_phase5, OpenMissingFileReturnsNull) {
    const char payload[] = "x";
    p5_ctx = BuildSingleFileCtx("app.wasm", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    TEST_ASSERT_NULL(TarFs_Open(p5_ctx, "/missing.txt", VFS_O_RDONLY));
}

TEST(tarfs_phase5, StatReportsRegularFileAndSize) {
    const char payload[] = "abcde";
    p5_ctx = BuildSingleFileCtx("app.wasm", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    void *h = TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    vfs_stat_t st;
    TEST_ASSERT_EQUAL_INT(0, TarFs_Stat(p5_ctx, h, &st));
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_REGULAR_FILE, st.filetype);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload) - 1, st.size);

    TarFs_Close(p5_ctx, h);
}

TEST(tarfs_phase5, SeekAndReadHonorPosition) {
    const char payload[] = "0123456789";
    p5_ctx = BuildSingleFileCtx("data.bin", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    void *h = TarFs_Open(p5_ctx, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    long pos = -1;
    TEST_ASSERT_EQUAL_INT(0, TarFs_Seek(p5_ctx, h, 4, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT(4, pos);

    char buf[6] = {0};
    int n = TarFs_Read(p5_ctx, h, buf, 3);
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("456", buf, 3);

    /* SEEK_CUR + over-end clamps to size, returns size. */
    TEST_ASSERT_EQUAL_INT(0, TarFs_Seek(p5_ctx, h, 100, VFS_SEEK_CUR, &pos));
    TEST_ASSERT_EQUAL_INT((long)(sizeof(payload) - 1), pos);

    /* Negative seek before start rejected. */
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          TarFs_Seek(p5_ctx, h, -100, VFS_SEEK_SET, &pos));

    TarFs_Close(p5_ctx, h);
}

static vfs_tarfs_ctx_t *BuildLayeredCtx(void) {
    /* Base layer: app.wasm + etc/config.json + etc/secret.txt + lib/x.so. */
    size_t off = 0;
    TarHeader(p5_baseLayer + off, "app.wasm", 4, '0');
    memcpy(p5_baseLayer + off + 512, "WASM", 4);
    off += 1024;
    TarHeader(p5_baseLayer + off, "etc/config.json", 5, '0');
    memcpy(p5_baseLayer + off + 512, "{a:1}", 5);
    off += 1024;
    TarHeader(p5_baseLayer + off, "etc/secret.txt", 6, '0');
    memcpy(p5_baseLayer + off + 512, "SECRET", 6);
    off += 1024;
    TarHeader(p5_baseLayer + off, "lib/x.so", 2, '0');
    memcpy(p5_baseLayer + off + 512, "ZZ", 2);
    /* Top layer: whiteout for etc/secret.txt. */
    TarHeader(p5_topLayer, "etc/.wh.secret.txt", 0, '0');

    uint8_t *layers[2] = {p5_topLayer, p5_baseLayer};
    size_t lens[2] = {sizeof(p5_topLayer), sizeof(p5_baseLayer)};
    return TarFsInit(layers, lens, 2);
}

static const vfs_dirent_t *NextDirent(const uint8_t *buf, size_t bufUsed,
                                      size_t *cursor, const char **name_out) {
    if (*cursor + sizeof(vfs_dirent_t) > bufUsed)
        return NULL;
    const vfs_dirent_t *d = (const vfs_dirent_t *)(buf + *cursor);
    *name_out = (const char *)(buf + *cursor + sizeof(vfs_dirent_t));
    *cursor += sizeof(vfs_dirent_t) + d->d_namlen;
    return d;
}

TEST(tarfs_phase5, ReadDirRootListsFilesAndImplicitDirs) {
    p5_ctx = BuildLayeredCtx();
    TEST_ASSERT_NOT_NULL(p5_ctx);

    void *h = TarFs_Open(p5_ctx, "/", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_NOT_NULL(h);

    uint8_t buf[256] = {0};
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, TarFs_ReadDir(p5_ctx, h, buf, sizeof(buf), &cookie, &used));

    /* Index sorted by full path:
     *   app.wasm, etc/.wh.secret.txt, etc/config.json, lib/x.so
     * Root listing should yield: app.wasm (file), etc (dir), lib (dir). */
    size_t cur = 0;
    const char *name;
    const vfs_dirent_t *d;

    d = NextDirent(buf, used, &cur, &name);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_REGULAR_FILE, d->d_type);
    TEST_ASSERT_EQUAL_UINT32(8, d->d_namlen);
    TEST_ASSERT_EQUAL_STRING_LEN("app.wasm", name, 8);

    d = NextDirent(buf, used, &cur, &name);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_DIRECTORY, d->d_type);
    TEST_ASSERT_EQUAL_UINT32(3, d->d_namlen);
    TEST_ASSERT_EQUAL_STRING_LEN("etc", name, 3);

    d = NextDirent(buf, used, &cur, &name);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_DIRECTORY, d->d_type);
    TEST_ASSERT_EQUAL_UINT32(3, d->d_namlen);
    TEST_ASSERT_EQUAL_STRING_LEN("lib", name, 3);

    TEST_ASSERT_EQUAL_size_t(used, cur);

    TarFs_Close(p5_ctx, h);
}

TEST(tarfs_phase5, OpenImplicitSubdirAndListChildren) {
    p5_ctx = BuildLayeredCtx();
    TEST_ASSERT_NOT_NULL(p5_ctx);

    void *h = TarFs_Open(p5_ctx, "/etc", VFS_O_RDONLY | VFS_O_DIRECTORY);
    TEST_ASSERT_NOT_NULL(h);

    uint8_t buf[256] = {0};
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, TarFs_ReadDir(p5_ctx, h, buf, sizeof(buf), &cookie, &used));

    /* etc/ children: config.json (file). secret.txt is shadowed by the
     * top-layer whiteout in `etc/.wh.secret.txt`, which itself never appears
     * in the listing. */
    size_t cur = 0;
    const char *name;
    const vfs_dirent_t *d = NextDirent(buf, used, &cur, &name);
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT(VFS_FILETYPE_REGULAR_FILE, d->d_type);
    TEST_ASSERT_EQUAL_UINT32(11, d->d_namlen);
    TEST_ASSERT_EQUAL_STRING_LEN("config.json", name, 11);

    /* No further entries (secret.txt suppressed, .wh.secret.txt is a
     * whiteout marker in the index — TARFS_WHITEOUT entry is skipped). */
    TEST_ASSERT_EQUAL_size_t(used, cur);

    TarFs_Close(p5_ctx, h);
}

TEST(tarfs_phase5, WhiteoutHidesShadowedFile) {
    p5_ctx = BuildLayeredCtx();
    TEST_ASSERT_NOT_NULL(p5_ctx);

    /* etc/secret.txt is shadowed by etc/.wh.secret.txt -> open returns NULL. */
    TEST_ASSERT_NULL(TarFs_Open(p5_ctx, "/etc/secret.txt", VFS_O_RDONLY));

    /* The shadowing-but-not-shadowed file remains accessible. */
    void *h = TarFs_Open(p5_ctx, "/etc/config.json", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);
    TarFs_Close(p5_ctx, h);
}

TEST(tarfs_phase5, OpenDirectoryWithODirectoryFlagOnFileFails) {
    const char payload[] = "x";
    p5_ctx = BuildSingleFileCtx("app.wasm", payload, sizeof(payload) - 1);
    TEST_ASSERT_NOT_NULL(p5_ctx);

    /* O_DIRECTORY against a regular file must fail. */
    TEST_ASSERT_NULL(
        TarFs_Open(p5_ctx, "/app.wasm", VFS_O_RDONLY | VFS_O_DIRECTORY));
}

TEST_GROUP_RUNNER(tarfs_phase5) {
    RUN_TEST_CASE(tarfs_phase5, OpenReadsRegularFileAndStops);
    RUN_TEST_CASE(tarfs_phase5, OpenRejectsWriteFlags);
    RUN_TEST_CASE(tarfs_phase5, OpenMissingFileReturnsNull);
    RUN_TEST_CASE(tarfs_phase5, StatReportsRegularFileAndSize);
    RUN_TEST_CASE(tarfs_phase5, SeekAndReadHonorPosition);
    RUN_TEST_CASE(tarfs_phase5, ReadDirRootListsFilesAndImplicitDirs);
    RUN_TEST_CASE(tarfs_phase5, OpenImplicitSubdirAndListChildren);
    RUN_TEST_CASE(tarfs_phase5, WhiteoutHidesShadowedFile);
    RUN_TEST_CASE(tarfs_phase5, OpenDirectoryWithODirectoryFlagOnFileFails);
}
