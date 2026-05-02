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

/***************************************/
/* End-to-end opens through VfsOpen with c->tarfs populated. */
/***************************************/

/* Two layer buffers shared across fixtures; tear-down zeroes them so each
 * test sees a clean slate. */
static uint8_t p7_layerA[512 * 4];
static uint8_t p7_layerB[512 * 4];
static vfs_ctx_t p7_vfs;

static vfs_ctx_t BuildVfsWithTarfs(uint8_t *const layers[],
                                   const size_t layer_lens[],
                                   uint8_t layer_cnt) {
    vfs_tarfs_ctx_t *t = TarFsInit(layers, layer_lens, layer_cnt);
    if (!t)
        return NULL;
    vfs_ctx_t v = VfsInit();
    if (!v) {
        TarFsDestroy(t);
        return NULL;
    }
    /* Ownership transfers to the vfs ctx — VfsDestroy frees the tarfs. */
    VfsAttachTarfs(v, t);
    return v;
}

static int ReadAll(vfs_ctx_t v, int fd, void *buf, size_t cap) {
    int n = VfsRead(v, fd, buf, cap);
    return n;
}

TEST_GROUP(tarfs_single_layer);

TEST_SETUP(tarfs_single_layer) {
    memset(p7_layerA, 0, sizeof(p7_layerA));
    p7_vfs = NULL;
}

TEST_TEAR_DOWN(tarfs_single_layer) {
    if (p7_vfs)
        VfsDestroy(&p7_vfs);
}

TEST(tarfs_single_layer, OpensAppWasmAndReadsPayload) {
    const char payload[] = "WASM-PAYLOAD";
    TarHeader(p7_layerA, "app.wasm", sizeof(payload) - 1, '0');
    memcpy(p7_layerA + 512, payload, sizeof(payload) - 1);

    uint8_t *layers[1] = {p7_layerA};
    size_t lens[1] = {sizeof(p7_layerA)};
    p7_vfs = BuildVfsWithTarfs(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(p7_vfs);

    int fd = VfsOpen(p7_vfs, "/app.wasm", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[32] = {0};
    int n = ReadAll(p7_vfs, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT((int)(sizeof(payload) - 1), n);
    TEST_ASSERT_EQUAL_STRING_LEN(payload, buf, sizeof(payload) - 1);

    TEST_ASSERT_EQUAL_INT(0, VfsClose(p7_vfs, fd));
}

TEST_GROUP_RUNNER(tarfs_single_layer) {
    RUN_TEST_CASE(tarfs_single_layer, OpensAppWasmAndReadsPayload);
}

TEST_GROUP(tarfs_layer_override);

TEST_SETUP(tarfs_layer_override) {
    memset(p7_layerA, 0, sizeof(p7_layerA));
    memset(p7_layerB, 0, sizeof(p7_layerB));
    p7_vfs = NULL;
}

TEST_TEAR_DOWN(tarfs_layer_override) {
    if (p7_vfs)
        VfsDestroy(&p7_vfs);
}

TEST(tarfs_layer_override, NewerLayerWins) {
    /* Base layer carries the original file; top layer ships an override. */
    TarHeader(p7_layerB, "data.txt", 4, '0');
    memcpy(p7_layerB + 512, "base", 4);
    TarHeader(p7_layerA, "data.txt", 8, '0');
    memcpy(p7_layerA + 512, "override", 8);

    /* layers[0] is newest. */
    uint8_t *layers[2] = {p7_layerA, p7_layerB};
    size_t lens[2] = {sizeof(p7_layerA), sizeof(p7_layerB)};
    p7_vfs = BuildVfsWithTarfs(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(p7_vfs);

    int fd = VfsOpen(p7_vfs, "/data.txt", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    char buf[16] = {0};
    int n = ReadAll(p7_vfs, fd, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(8, n);
    TEST_ASSERT_EQUAL_STRING_LEN("override", buf, 8);

    TEST_ASSERT_EQUAL_INT(0, VfsClose(p7_vfs, fd));
}

TEST_GROUP_RUNNER(tarfs_layer_override) {
    RUN_TEST_CASE(tarfs_layer_override, NewerLayerWins);
}

TEST_GROUP(tarfs_whiteout);

TEST_SETUP(tarfs_whiteout) {
    memset(p7_layerA, 0, sizeof(p7_layerA));
    memset(p7_layerB, 0, sizeof(p7_layerB));
    p7_vfs = NULL;
}

TEST_TEAR_DOWN(tarfs_whiteout) {
    if (p7_vfs)
        VfsDestroy(&p7_vfs);
}

TEST(tarfs_whiteout, ShadowedFileReturnsENOENT) {
    /* Base layer has secret.txt; top layer whites it out. */
    TarHeader(p7_layerB, "secret.txt", 6, '0');
    memcpy(p7_layerB + 512, "SECRET", 6);
    TarHeader(p7_layerA, ".wh.secret.txt", 0, '0');

    uint8_t *layers[2] = {p7_layerA, p7_layerB};
    size_t lens[2] = {sizeof(p7_layerA), sizeof(p7_layerB)};
    p7_vfs = BuildVfsWithTarfs(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(p7_vfs);

    int fd = VfsOpen(p7_vfs, "/secret.txt", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-ENOENT, fd);
}

TEST_GROUP_RUNNER(tarfs_whiteout) {
    RUN_TEST_CASE(tarfs_whiteout, ShadowedFileReturnsENOENT);
}

/***************************************/
/* PAX and GNU long-name compatibility  */
/***************************************/

/* Path that exceeds the 100-char ustar name field limit. */
#define PAX_LONG_PATH \
    "level01/level02/level03/level04/level05/level06/" \
    "level07/level08/level09/level10/level11/level12/data.txt"

/* Compute the self-referential PAX record length for "path=<path>\n". */
static size_t PaxRecordLen(size_t path_len) {
    for (int digits = 1; digits <= 10; digits++) {
        size_t n = (size_t)digits + 1 + 5 + path_len + 1;
        size_t tmp = n;
        int nd = 0;
        do { nd++; tmp /= 10; } while (tmp > 0);
        if (nd == digits)
            return n;
    }
    return 0;
}

/* Build a PAX 'x' header + data block into out[1024].
 * Optionally appends a size= record when pax_size > 0. */
static void BuildPaxBlock(uint8_t *out, const char *path, uint32_t pax_size) {
    char pax_data[512];
    memset(pax_data, 0, sizeof(pax_data));
    size_t path_len = strlen(path);
    size_t rec_len = PaxRecordLen(path_len);
    int written = snprintf(pax_data, sizeof(pax_data), "%zu path=%s\n",
                           rec_len, path);
    if (pax_size > 0) {
        /* Append "NN size=DDDD\n" */
        char size_rec[64];
        size_t sz_val_len = (size_t)snprintf(size_rec + 16, 48, "%u", pax_size);
        size_t sz_rec_len = PaxRecordLen(sz_val_len);
        /* overwrite the "size=" key length calculation: digits+1+5+val+1 */
        for (int d = 1; d <= 6; d++) {
            size_t n = (size_t)d + 1 + 5 + sz_val_len + 1;
            size_t tmp = n; int nd = 0;
            do { nd++; tmp /= 10; } while (tmp > 0);
            if (nd == d) { sz_rec_len = n; break; }
        }
        snprintf(pax_data + written, sizeof(pax_data) - (size_t)written,
                 "%zu size=%u\n", sz_rec_len, pax_size);
        written = (int)strlen(pax_data);
    }

    memset(out, 0, 1024);
    TarHeader(out, "PaxHeader", (uint32_t)written, 'x');
    memcpy(out + 512, pax_data, (size_t)written);
}

/* Build a GNU 'L' long-name block into out[1024]. */
static void BuildGnuLBlock(uint8_t *out, const char *path) {
    size_t path_len = strlen(path) + 1; /* include NUL */
    memset(out, 0, 1024);
    TarHeader(out, "././@LongLink", (uint32_t)path_len, 'L');
    memcpy(out + 512, path, path_len);
}

static uint8_t pc_layer[512 * 12]; /* enough for pax+data+file+data+2 zero blks */
static uint8_t pc_layerB[512 * 8];
static vfs_tarfs_ctx_t *pc_ctx;

TEST_GROUP(tarfs_pax_compat);

TEST_SETUP(tarfs_pax_compat) {
    memset(pc_layer, 0, sizeof(pc_layer));
    memset(pc_layerB, 0, sizeof(pc_layerB));
    pc_ctx = NULL;
}

TEST_TEAR_DOWN(tarfs_pax_compat) {
    if (pc_ctx) {
        TarFsDestroy(pc_ctx);
        pc_ctx = NULL;
    }
}

TEST(tarfs_pax_compat, PaxLongPathIndexed) {
    /* PAX 'x' block + actual file entry with a >100-char path. */
    BuildPaxBlock(pc_layer, PAX_LONG_PATH, 0);
    /* File entry immediately follows the 1024-byte PAX block. */
    TarHeader(pc_layer + 1024, "truncated", 5, '0');
    memcpy(pc_layer + 1536, "hello", 5);

    uint8_t *layers[1] = {pc_layer};
    size_t lens[1] = {sizeof(pc_layer)};
    pc_ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(pc_ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(pc_ctx));

    void *h = TarFs_Open(pc_ctx, "/" PAX_LONG_PATH, VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(5, TarFs_Read(pc_ctx, h, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("hello", buf, 5);

    TarFs_Close(pc_ctx, h);
}

TEST(tarfs_pax_compat, PaxLongPathWhiteout) {
    /* PAX entry whose path= is a whiteout for a file in the base layer. */
    static uint8_t base[512 * 4];
    memset(base, 0, sizeof(base));
    TarHeader(base, "secret.bin", 3, '0');
    memcpy(base + 512, "TOP", 3);

    /* Top layer: PAX whiteout for secret.bin */
    char wh_path[] = ".wh.secret.bin";
    BuildPaxBlock(pc_layer, wh_path, 0);
    TarHeader(pc_layer + 1024, "truncated", 0, '0');

    uint8_t *layers[2] = {pc_layer, base};
    size_t lens[2] = {sizeof(pc_layer), sizeof(base)};
    pc_ctx = TarFsInit(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(pc_ctx);

    void *h = TarFs_Open(pc_ctx, "/secret.bin", VFS_O_RDONLY);
    TEST_ASSERT_NULL(h);
}

TEST(tarfs_pax_compat, GnuLongNameIndexed) {
    /* GNU 'L' block + actual file entry with a >100-char path. */
    BuildGnuLBlock(pc_layer, PAX_LONG_PATH);
    TarHeader(pc_layer + 1024, "truncated", 4, '0');
    memcpy(pc_layer + 1536, "GNU!", 4);

    uint8_t *layers[1] = {pc_layer};
    size_t lens[1] = {sizeof(pc_layer)};
    pc_ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(pc_ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(pc_ctx));

    void *h = TarFs_Open(pc_ctx, "/" PAX_LONG_PATH, VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(4, TarFs_Read(pc_ctx, h, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("GNU!", buf, 4);

    TarFs_Close(pc_ctx, h);
}

TEST(tarfs_pax_compat, GnuLongNameShadowedByPax) {
    /* Base layer: file via GNU 'L'. Top layer: PAX entry shadows it. */
    BuildGnuLBlock(pc_layerB, PAX_LONG_PATH);
    TarHeader(pc_layerB + 1024, "truncated", 4, '0');
    memcpy(pc_layerB + 1536, "BASE", 4);

    BuildPaxBlock(pc_layer, PAX_LONG_PATH, 0);
    TarHeader(pc_layer + 1024, "truncated", 3, '0');
    memcpy(pc_layer + 1536, "TOP", 3);

    uint8_t *layers[2] = {pc_layer, pc_layerB};
    size_t lens[2] = {sizeof(pc_layer), sizeof(pc_layerB)};
    pc_ctx = TarFsInit(layers, lens, 2);
    TEST_ASSERT_NOT_NULL(pc_ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(pc_ctx));

    void *h = TarFs_Open(pc_ctx, "/" PAX_LONG_PATH, VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    char buf[8] = {0};
    int n = TarFs_Read(pc_ctx, h, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_STRING_LEN("TOP", buf, 3);

    TarFs_Close(pc_ctx, h);
}

TEST(tarfs_pax_compat, PaxSizeOverride) {
    /* PAX size= overrides ustar size field in the index. */
    BuildPaxBlock(pc_layer, "data.bin", 3); /* PAX says size=3 */
    /* ustar entry says size=5 — PAX wins for the stored index size. */
    TarHeader(pc_layer + 1024, "data.bin", 5, '0');
    memcpy(pc_layer + 1536, "ABCDE", 5);

    uint8_t *layers[1] = {pc_layer};
    size_t lens[1] = {sizeof(pc_layer)};
    pc_ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(pc_ctx);

    void *h = TarFs_Open(pc_ctx, "/data.bin", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);

    vfs_stat_t st;
    TarFs_Stat(pc_ctx, h, &st);
    TEST_ASSERT_EQUAL_UINT32(3, st.size);

    char buf[8] = {0};
    TEST_ASSERT_EQUAL_INT(3, TarFs_Read(pc_ctx, h, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING_LEN("ABC", buf, 3);

    TarFs_Close(pc_ctx, h);
}

TEST(tarfs_pax_compat, GlobalHeaderSkipped) {
    /* 'g' block must not consume the next entry's path override. */
    memset(pc_layer, 0, sizeof(pc_layer));
    /* 'g' block with size=0 (empty global header). */
    TarHeader(pc_layer, "global", 0, 'g');
    /* Normal entry immediately follows. */
    TarHeader(pc_layer + 512, "present.txt", 2, '0');
    memcpy(pc_layer + 1024, "OK", 2);

    uint8_t *layers[1] = {pc_layer};
    size_t lens[1] = {sizeof(pc_layer)};
    pc_ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(pc_ctx);
    TEST_ASSERT_EQUAL_UINT16(1, TarFsIndexLen(pc_ctx));

    void *h = TarFs_Open(pc_ctx, "/present.txt", VFS_O_RDONLY);
    TEST_ASSERT_NOT_NULL(h);
    TarFs_Close(pc_ctx, h);
}

TEST(tarfs_pax_compat, PaxAppWasmPrefetch) {
    /* app.wasm delivered via PAX format entry still triggers pre-fetch. */
    const char payload[] = "WASM";
    BuildPaxBlock(pc_layer, "app.wasm", 0);
    TarHeader(pc_layer + 1024, "app.wasm", sizeof(payload) - 1, '0');
    memcpy(pc_layer + 1536, payload, sizeof(payload) - 1);

    uint8_t *layers[1] = {pc_layer};
    size_t lens[1] = {sizeof(pc_layer)};
    pc_ctx = TarFsInit(layers, lens, 1);
    TEST_ASSERT_NOT_NULL(pc_ctx);

    size_t wlen = 0;
    const uint8_t *wasm = TarFsEntrypointWasm(pc_ctx, &wlen);
    TEST_ASSERT_NOT_NULL(wasm);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload) - 1, wlen);
    TEST_ASSERT_EQUAL_MEMORY(payload, wasm, sizeof(payload) - 1);
}

TEST_GROUP_RUNNER(tarfs_pax_compat) {
    RUN_TEST_CASE(tarfs_pax_compat, PaxLongPathIndexed);
    RUN_TEST_CASE(tarfs_pax_compat, PaxLongPathWhiteout);
    RUN_TEST_CASE(tarfs_pax_compat, GnuLongNameIndexed);
    RUN_TEST_CASE(tarfs_pax_compat, GnuLongNameShadowedByPax);
    RUN_TEST_CASE(tarfs_pax_compat, PaxSizeOverride);
    RUN_TEST_CASE(tarfs_pax_compat, GlobalHeaderSkipped);
    RUN_TEST_CASE(tarfs_pax_compat, PaxAppWasmPrefetch);
}
