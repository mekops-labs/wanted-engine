#include "unity_fixture.h"

#include <stdint.h>
#include <string.h>

#include <vfs-tarfs.h>

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
