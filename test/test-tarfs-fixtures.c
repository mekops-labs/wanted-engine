/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform.h>
#include <vfs-tarfs.h>
#include <vfs.h>

/* tarfs_fixtures — layers exported from images that an unmodified `podman
 * build` made. test/fixtures/layers/make-fixtures.sh rebuilds them. */

TEST_GROUP(tarfs_fixtures);

#define MAX_FIXTURE_LAYERS 3
#define SHA_HEX 64

static uint8_t *layerData[MAX_FIXTURE_LAYERS];
static size_t layerLen[MAX_FIXTURE_LAYERS];
static vfs_tarfs_ctx_t *ctx;

TEST_SETUP(tarfs_fixtures) {
    memset(layerData, 0, sizeof(layerData));
    memset(layerLen, 0, sizeof(layerLen));
    ctx = NULL;
}

TEST_TEAR_DOWN(tarfs_fixtures) {
    TarFsDestroy(ctx);
    for (int i = 0; i < MAX_FIXTURE_LAYERS; i++)
        free(layerData[i]);
}

static uint8_t *readFile(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;

    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_END));
    *len = (size_t)ftell(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, 0, SEEK_SET));
    buf = malloc(*len);
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT32(*len, fread(buf, 1, *len, f));
    fclose(f);
    return buf;
}

static void loadFixture(const char *name) {
    char path[256];

    for (int i = 0; i < MAX_FIXTURE_LAYERS; i++) {
        snprintf(path, sizeof(path), "%s/layers/%s/layer-%d.tar",
                 WANTED_FIXTURE_DIR, name, i);
        layerData[i] = readFile(path, &layerLen[i]);
    }
}

/* Layers sit in manifest order, base first; TarFS takes the topmost first, so
 * `baseOnTop` mounts them the wrong way round. */
static void mountOrder(bool baseOnTop) {
    uint8_t *layers[MAX_FIXTURE_LAYERS];
    size_t lens[MAX_FIXTURE_LAYERS];

    for (int i = 0; i < MAX_FIXTURE_LAYERS; i++) {
        int src = baseOnTop ? i : MAX_FIXTURE_LAYERS - 1 - i;

        layers[i] = layerData[src];
        lens[i] = layerLen[src];
    }
    ctx = TarFsInit(layers, lens, MAX_FIXTURE_LAYERS);
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
    char buf[32] = {0};
    void *h = TarFs_Open(ctx, path, VFS_O_RDONLY);

    TEST_ASSERT_NOT_NULL_MESSAGE(h, path);
    TEST_ASSERT_EQUAL_INT((int)strlen(want),
                          TarFs_Read(ctx, h, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TarFs_Close(ctx, h);
}

static void hex(const uint8_t digest[PLATFORM_SHA256_DIGEST_LEN], char *out) {
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < PLATFORM_SHA256_DIGEST_LEN; i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[(2 * i) + 1] = digits[digest[i] & 0x0f];
    }
    out[SHA_HEX] = '\0';
}

TEST(tarfs_fixtures, ThreeCopySteps_MergeAsTheImageWasBuilt) {
    loadFixture("plain");
    mountOrder(false);

    expectContent("app.wasm", "wasm-code");
    expectContent("data/one.txt", "one");
    expectContent("data/sub/two.txt", "two");
    expectContent("config.json", "cfg");
    TEST_ASSERT_EQUAL_UINT16(0, TarFsSkipped(ctx));
}

TEST(tarfs_fixtures, WhiteoutAndOpaqueDirectory_BehaveAsTheBuildWroteThem) {
    size_t len = 0;
    const uint8_t *wasm;

    loadFixture("mask");
    mountOrder(false);

    expectContent("app.wasm", "wasm");
    expectContent("new.txt", "n");
    expectContent("top.txt", "top");
    expectContent("data/fresh.txt", "fresh");
    TEST_ASSERT_FALSE(exists("etc/motd"));
    TEST_ASSERT_FALSE(exists("data/a.txt"));
    TEST_ASSERT_FALSE(exists("data/sub/b.txt"));
    TEST_ASSERT_FALSE(exists("etc"));
    wasm = TarFsEntrypointWasm(ctx, &len);
    TEST_ASSERT_NOT_NULL(wasm);
    TEST_ASSERT_EQUAL_UINT32(4, len);
}

TEST(tarfs_fixtures, ABaseOnTop_ShowsAnOrderMistake) {
    loadFixture("mask");
    mountOrder(true);

    TEST_ASSERT_TRUE(exists("etc/motd"));
    TEST_ASSERT_TRUE(exists("data/a.txt"));
}

TEST(tarfs_fixtures, LayerDigests_AreTheDiffIdsOfTheImageConfig) {
    static const char *names[] = {"plain", "mask"};
    char path[256];
    char want[SHA_HEX + 2];

    for (size_t n = 0; n < 2; n++) {
        FILE *ids;

        loadFixture(names[n]);
        snprintf(path, sizeof(path), "%s/layers/%s/diff_ids.txt",
                 WANTED_FIXTURE_DIR, names[n]);
        ids = fopen(path, "r");
        TEST_ASSERT_NOT_NULL(ids);
        for (int i = 0; i < MAX_FIXTURE_LAYERS; i++) {
            uint8_t digest[PLATFORM_SHA256_DIGEST_LEN];
            char got[SHA_HEX + 1];
            void *sha = PlatformSha256New();

            TEST_ASSERT_NOT_NULL(fgets(want, sizeof(want), ids));
            want[SHA_HEX] = '\0';
            PlatformSha256Update(sha, layerData[i], layerLen[i]);
            PlatformSha256Final(sha, digest);
            PlatformSha256Free(sha);
            hex(digest, got);
            TEST_ASSERT_EQUAL_STRING(want, got);
        }
        fclose(ids);
        for (int i = 0; i < MAX_FIXTURE_LAYERS; i++) {
            free(layerData[i]);
            layerData[i] = NULL;
        }
    }
}

TEST_GROUP_RUNNER(tarfs_fixtures) {
    RUN_TEST_CASE(tarfs_fixtures, ThreeCopySteps_MergeAsTheImageWasBuilt);
    RUN_TEST_CASE(tarfs_fixtures,
                  WhiteoutAndOpaqueDirectory_BehaveAsTheBuildWroteThem);
    RUN_TEST_CASE(tarfs_fixtures, ABaseOnTop_ShowsAnOrderMistake);
    RUN_TEST_CASE(tarfs_fixtures, LayerDigests_AreTheDiffIdsOfTheImageConfig);
}
