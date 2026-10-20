/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs-tarfs.h>
#include <vfs.h>

#include "test-utils.h"

/* tarfs_layers — how layers merge: shadowing, whiteouts, opaque directories,
 * and the entries TarFS cannot serve. */

TEST_GROUP(tarfs_layers);

#define LAYER_BYTES (512 * 24)
#define MAX_TEST_LAYERS 4
#define MAX_NAMES 16

typedef struct {
    const char *name;
    char type;
    const char *data;
} ent_t;

static uint8_t layerBuf[MAX_TEST_LAYERS][LAYER_BYTES];
static size_t layerLen[MAX_TEST_LAYERS];
static vfs_tarfs_ctx_t *ctx;

TEST_SETUP(tarfs_layers) {
    memset(layerBuf, 0, sizeof(layerBuf));
    memset(layerLen, 0, sizeof(layerLen));
    ctx = NULL;
}

TEST_TEAR_DOWN(tarfs_layers) {
    TarFsDestroy(ctx);
    ctx = NULL;
}

/* One layer from a list of entries: header, padded data, end-of-archive. */
static void buildLayer(int idx, const ent_t *e, size_t n) {
    uint8_t *out = layerBuf[idx];
    size_t off = 0;

    for (size_t i = 0; i < n; i++) {
        size_t len = e[i].data != NULL ? strlen(e[i].data) : 0;

        TarHeader(out + off, e[i].name, (uint32_t)len,
                  e[i].type != '\0' ? e[i].type : '0');
        if (len > 0)
            memcpy(out + off + 512, e[i].data, len);
        off += 512 + ((len + 511) & ~(size_t)511);
    }
    off += 1024;
    TEST_ASSERT_TRUE(off <= LAYER_BYTES);
    layerLen[idx] = off;
}

/* The merged tree of `n` layers, topmost first. */
static void mount(int n) {
    uint8_t *layers[MAX_TEST_LAYERS];
    size_t lens[MAX_TEST_LAYERS];

    for (int i = 0; i < n; i++) {
        layers[i] = layerBuf[i];
        lens[i] = layerLen[i];
    }
    ctx = TarFsInit(layers, lens, (uint8_t)n);
    TEST_ASSERT_NOT_NULL(ctx);
}

static bool exists(const char *path) {
    void *h = TarFs_Open(ctx, path, VFS_O_RDONLY);

    if (h == NULL)
        return false;
    TarFs_Close(ctx, h);
    return true;
}

static void expectContent(const char *path, const char *want) {
    char buf[64] = {0};
    void *h = TarFs_Open(ctx, path, VFS_O_RDONLY);

    TEST_ASSERT_NOT_NULL_MESSAGE(h, path);
    TEST_ASSERT_EQUAL_INT((int)strlen(want),
                          TarFs_Read(ctx, h, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TarFs_Close(ctx, h);
}

/* Names TarFs_ReadDir lists under `path`, sorted as the index keeps them. */
static size_t listDir(const char *path, char names[MAX_NAMES][32]) {
    uint8_t buf[1024];
    uint64_t cookie = 0;
    size_t used = 0;
    size_t n = 0;
    void *h = TarFs_Open(ctx, path, VFS_O_RDONLY);

    TEST_ASSERT_NOT_NULL_MESSAGE(h, path);
    TEST_ASSERT_EQUAL_INT(
        0, TarFs_ReadDir(ctx, h, buf, sizeof(buf), &cookie, &used));
    for (size_t off = 0; off < used;) {
        vfs_dirent_t d;

        memcpy(&d, buf + off, sizeof(d));
        TEST_ASSERT_TRUE(n < MAX_NAMES && d.d_namlen < 32);
        memcpy(names[n], buf + off + sizeof(d), d.d_namlen);
        names[n][d.d_namlen] = '\0';
        n++;
        off += sizeof(d) + d.d_namlen;
    }
    TarFs_Close(ctx, h);
    return n;
}

TEST(tarfs_layers, OpaqueDirectory_HidesEveryOlderEntryUnderIt) {
    char names[MAX_NAMES][32];
    static const ent_t base[] = {
        {"d/a", 0, "old-a"}, {"d/sub/b", 0, "old-b"}, {"e/c", 0, "kept"}};
    static const ent_t top[] = {{"d/.wh..wh..opq", 0, NULL},
                                {"d/new", 0, "fresh"}};

    buildLayer(0, top, 2);
    buildLayer(1, base, 3);
    mount(2);

    TEST_ASSERT_FALSE(exists("d/a"));
    TEST_ASSERT_FALSE(exists("d/sub/b"));
    TEST_ASSERT_FALSE(exists("d/sub"));
    expectContent("d/new", "fresh");
    expectContent("e/c", "kept");

    TEST_ASSERT_EQUAL_UINT32(1, listDir("d", names));
    TEST_ASSERT_EQUAL_STRING("new", names[0]);
    TEST_ASSERT_EQUAL_UINT32(2, listDir("", names));
}

TEST(tarfs_layers, OpaqueDirectory_KeepsTheEntriesOfItsOwnLayer) {
    char names[MAX_NAMES][32];
    static const ent_t top[] = {
        {"d/x", 0, "x"}, {"d/.wh..wh..opq", 0, NULL}, {"d/y", 0, "y"}};

    buildLayer(0, top, 3);
    mount(1);

    expectContent("d/x", "x");
    expectContent("d/y", "y");
    TEST_ASSERT_EQUAL_UINT32(2, listDir("d", names));
}

TEST(tarfs_layers, OpaqueRoot_HidesTheWholeOlderTree_EntrypointIncluded) {
    static const ent_t base[] = {{"app.wasm", 0, "old-wasm"}, {"d/a", 0, "a"}};
    static const ent_t top[] = {{".wh..wh..opq", 0, NULL},
                                {"app.wasm", 0, "new-wasm"}};
    const uint8_t *wasm;
    size_t len = 0;

    buildLayer(0, top, 2);
    buildLayer(1, base, 2);
    mount(2);

    TEST_ASSERT_FALSE(exists("d/a"));
    expectContent("app.wasm", "new-wasm");
    wasm = TarFsEntrypointWasm(ctx, &len);
    TEST_ASSERT_EQUAL_UINT32(8, len);
    TEST_ASSERT_EQUAL_MEMORY("new-wasm", wasm, 8);
}

TEST(tarfs_layers, DirectoryWhiteout_HidesTheDirectoryAndItsSubtree) {
    char names[MAX_NAMES][32];
    static const ent_t base[] = {
        {"d/a", 0, "a"}, {"d/sub/b", 0, "b"}, {"keep", 0, "k"}};
    static const ent_t top[] = {{".wh.d", 0, NULL}};

    buildLayer(0, top, 1);
    buildLayer(1, base, 3);
    mount(2);

    TEST_ASSERT_FALSE(exists("d"));
    TEST_ASSERT_FALSE(exists("d/"));
    TEST_ASSERT_FALSE(exists("d/a"));
    TEST_ASSERT_FALSE(exists("d/sub/b"));
    expectContent("keep", "k");
    TEST_ASSERT_EQUAL_UINT32(1, listDir("", names));
    TEST_ASSERT_EQUAL_STRING("keep", names[0]);
}

TEST(tarfs_layers, Whiteout_NeverHidesAnEntryOfItsOwnLayer) {
    char names[MAX_NAMES][32];
    static const ent_t base[] = {{"d/old", 0, "old"}};
    static const ent_t top[] = {{".wh.d", 0, NULL}, {"d/own", 0, "own"}};

    buildLayer(0, top, 2);
    buildLayer(1, base, 1);
    mount(2);

    expectContent("d/own", "own");
    TEST_ASSERT_FALSE(exists("d/old"));
    TEST_ASSERT_TRUE(exists("d"));
    TEST_ASSERT_EQUAL_UINT32(1, listDir("d", names));
    TEST_ASSERT_EQUAL_STRING("own", names[0]);
}

TEST(tarfs_layers, FileAndItsWhiteoutInOneLayer_TheFileWins) {
    static const ent_t topA[] = {{".wh.x", 0, NULL}, {"x", 0, "kept"}};
    static const ent_t topB[] = {{"x", 0, "kept"}, {".wh.x", 0, NULL}};
    static const ent_t base[] = {{"x", 0, "old"}};

    buildLayer(0, topA, 2);
    buildLayer(1, base, 1);
    mount(2);
    expectContent("x", "kept");

    TarFsDestroy(ctx);
    ctx = NULL;
    buildLayer(0, topB, 2);
    mount(2);
    expectContent("x", "kept");
}

TEST(tarfs_layers, MiddleWhiteout_HidesOnlyOlderLayers) {
    static const ent_t base[] = {{"f", 0, "base"}, {"g", 0, "base-g"}};
    static const ent_t mid[] = {{".wh.f", 0, NULL}, {".wh.g", 0, NULL}};
    static const ent_t top[] = {{"f", 0, "top"}};

    buildLayer(0, top, 1);
    buildLayer(1, mid, 2);
    buildLayer(2, base, 2);
    mount(3);

    expectContent("f", "top");
    TEST_ASSERT_FALSE(exists("g"));
}

TEST(tarfs_layers, MiddleOpaqueDirectory_HidesOlderAndAllowsNewer) {
    static const ent_t base[] = {{"d/a", 0, "a"}};
    static const ent_t mid[] = {{"d/.wh..wh..opq", 0, NULL}, {"d/m", 0, "m"}};
    static const ent_t top[] = {{"d/t", 0, "t"}};

    buildLayer(0, top, 1);
    buildLayer(1, mid, 2);
    buildLayer(2, base, 1);
    mount(3);

    TEST_ASSERT_FALSE(exists("d/a"));
    expectContent("d/m", "m");
    expectContent("d/t", "t");
}

TEST(tarfs_layers, NestedFileWhiteout_HidesOnlyThatFile) {
    static const ent_t base[] = {{"d/f", 0, "f"}, {"d/g", 0, "g"}};
    static const ent_t top[] = {{"d/.wh.f", 0, NULL}};

    buildLayer(0, top, 1);
    buildLayer(1, base, 2);
    mount(2);

    TEST_ASSERT_FALSE(exists("d/f"));
    expectContent("d/g", "g");
}

TEST(tarfs_layers, DirectoryHoldingOnlyMarkers_DoesNotExist) {
    char names[MAX_NAMES][32];
    static const ent_t base[] = {{"keep", 0, "k"}};
    static const ent_t top[] = {{"d/.wh..wh..opq", 0, NULL},
                                {"e/.wh.gone", 0, NULL}};

    buildLayer(0, top, 2);
    buildLayer(1, base, 1);
    mount(2);

    TEST_ASSERT_FALSE(exists("d"));
    TEST_ASSERT_FALSE(exists("e"));
    TEST_ASSERT_EQUAL_UINT32(1, listDir("", names));
}

TEST(tarfs_layers, LayerOrder_DecidesWhichContentWins_InBothDirections) {
    static const ent_t a[] = {{"f", 0, "from-a"}};
    static const ent_t b[] = {{"f", 0, "from-b"}};

    buildLayer(0, a, 1);
    buildLayer(1, b, 1);
    mount(2);
    expectContent("f", "from-a");

    TarFsDestroy(ctx);
    ctx = NULL;
    buildLayer(0, b, 1);
    buildLayer(1, a, 1);
    mount(2);
    expectContent("f", "from-b");
}

TEST(tarfs_layers, FourLayers_MergeNewestOverOldest) {
    static const ent_t l3[] = {{"a", 0, "3a"}, {"b", 0, "3b"}, {"c", 0, "3c"}};
    static const ent_t l2[] = {{"b", 0, "2b"}, {"c", 0, "2c"}};
    static const ent_t l1[] = {{".wh.c", 0, NULL}, {"d", 0, "1d"}};
    static const ent_t l0[] = {{"a", 0, "0a"}};

    buildLayer(0, l0, 1);
    buildLayer(1, l1, 2);
    buildLayer(2, l2, 2);
    buildLayer(3, l3, 3);
    mount(4);

    expectContent("a", "0a");
    expectContent("b", "2b");
    TEST_ASSERT_FALSE(exists("c"));
    expectContent("d", "1d");
}

TEST(tarfs_layers, SkippedEntries_AreCounted) {
    static const ent_t top[] = {{"hard", '1', NULL},  {"sym", '2', NULL},
                                {"chr", '3', NULL},   {"blk", '4', NULL},
                                {"fifo", '6', NULL},  {"dir", '5', NULL},
                                {"file", 0, "served"}};
    static const ent_t base[] = {{"old-link", '2', NULL}};

    buildLayer(0, top, 7);
    buildLayer(1, base, 1);
    mount(2);

    TEST_ASSERT_EQUAL_UINT16(6, TarFsSkipped(ctx));
    expectContent("file", "served");
    TEST_ASSERT_FALSE(exists("sym"));
}

TEST(tarfs_layers, NoSkippedEntries_CountsZero) {
    static const ent_t top[] = {{"a", 0, "a"}};

    buildLayer(0, top, 1);
    mount(1);
    TEST_ASSERT_EQUAL_UINT16(0, TarFsSkipped(ctx));
}

TEST_GROUP_RUNNER(tarfs_layers) {
    RUN_TEST_CASE(tarfs_layers, OpaqueDirectory_HidesEveryOlderEntryUnderIt);
    RUN_TEST_CASE(tarfs_layers, OpaqueDirectory_KeepsTheEntriesOfItsOwnLayer);
    RUN_TEST_CASE(tarfs_layers,
                  OpaqueRoot_HidesTheWholeOlderTree_EntrypointIncluded);
    RUN_TEST_CASE(tarfs_layers,
                  DirectoryWhiteout_HidesTheDirectoryAndItsSubtree);
    RUN_TEST_CASE(tarfs_layers, Whiteout_NeverHidesAnEntryOfItsOwnLayer);
    RUN_TEST_CASE(tarfs_layers, FileAndItsWhiteoutInOneLayer_TheFileWins);
    RUN_TEST_CASE(tarfs_layers, MiddleWhiteout_HidesOnlyOlderLayers);
    RUN_TEST_CASE(tarfs_layers, MiddleOpaqueDirectory_HidesOlderAndAllowsNewer);
    RUN_TEST_CASE(tarfs_layers, NestedFileWhiteout_HidesOnlyThatFile);
    RUN_TEST_CASE(tarfs_layers, DirectoryHoldingOnlyMarkers_DoesNotExist);
    RUN_TEST_CASE(tarfs_layers,
                  LayerOrder_DecidesWhichContentWins_InBothDirections);
    RUN_TEST_CASE(tarfs_layers, FourLayers_MergeNewestOverOldest);
    RUN_TEST_CASE(tarfs_layers, SkippedEntries_AreCounted);
    RUN_TEST_CASE(tarfs_layers, NoSkippedEntries_CountsZero);
}
