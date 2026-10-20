/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <registry-mem.h>
#include <registry.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* vfs_registry_tree — the root of the registry node, its blobs directory, and
 * the link route. */

extern const vfs_driver_t WantedRegistryDriver;

TEST_GROUP(vfs_registry_tree);

static const vfs_driver_t *drv;

TEST_SETUP(vfs_registry_tree) {
    DummyRegistryReset();
    drv = &WantedRegistryDriver;
    drv->Destroy((vfs_driver_t *)drv);
}

TEST_TEAR_DOWN(vfs_registry_tree) {
    drv->Destroy((vfs_driver_t *)drv);
    DummyRegistryReset();
}

static void sha(const char *bytes, char hex[65]) {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    void *ctx = PlatformSha256New();

    PlatformSha256Update(ctx, (const uint8_t *)bytes, strlen(bytes));
    PlatformSha256Final(ctx, digest);
    PlatformSha256Free(ctx);
    RegistryDigestToHex(digest, hex);
}

static void install(const char *ref, unsigned layer, const char *bytes) {
    size_t n = strlen(bytes);

    TEST_ASSERT_EQUAL_INT(
        (int)n,
        RegistryWriteLayer(START_WRITE, ref, layer, (const uint8_t *)bytes, n));
    TEST_ASSERT_EQUAL_INT(0,
                          RegistryWriteLayer(FINISH_WRITE, NULL, 0, NULL, 0));
}

/* Reads one directory into `names`, each name NUL-ended. Returns the count. */
static int listDir(const char *path, char names[][70], int max) {
    uint8_t buf[512];
    uint64_t cookie = 0;
    size_t used = 0;
    int count = 0;
    int fd = drv->Open(drv->ctx, path, VFS_O_RDONLY);

    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
    do {
        size_t at = 0;

        TEST_ASSERT_EQUAL_INT(
            0, drv->ReadDir(drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
        while (at < used && count < max) {
            vfs_dirent_t dir;

            memcpy(&dir, buf + at, sizeof(dir));
            at += sizeof(dir);
            memcpy(names[count], buf + at, dir.d_namlen);
            names[count][dir.d_namlen] = '\0';
            at += dir.d_namlen;
            count++;
        }
    } while (used > 0 && count < max);
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
    return count;
}

static void sendLink(const char *path, const char *payload, int expectClose) {
    int fd = drv->Open(drv->ctx, path, VFS_O_WRONLY);

    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
    TEST_ASSERT_EQUAL_INT((int)strlen(payload),
                          drv->Write(drv->ctx, fd, payload, strlen(payload)));
    TEST_ASSERT_EQUAL_INT(expectClose, drv->Close(drv->ctx, fd));
}

TEST(vfs_registry_tree, Root_ListsBlobsCapacityAndImages) {
    char names[8][70];

    TEST_ASSERT_EQUAL_INT(3, listDir("/", names, 8));
    TEST_ASSERT_EQUAL_STRING("blobs", names[0]);
    TEST_ASSERT_EQUAL_STRING("capacity", names[1]);
    TEST_ASSERT_EQUAL_STRING("images", names[2]);
    TEST_ASSERT_EQUAL_INT(3, listDir("", names, 8));
}

TEST(vfs_registry_tree, TheFlatPathsAreGone) {
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "app:1.0.0", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "reg_capacity", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "imagesx", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "capacity/x", VFS_O_RDONLY));
}

TEST(vfs_registry_tree, TheRootAndTheDirectoriesRefuseWrites) {
    TEST_ASSERT_EQUAL_INT(-EISDIR, drv->Open(drv->ctx, "/", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EISDIR, drv->Open(drv->ctx, "images", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EROFS, drv->Open(drv->ctx, "blobs", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EPERM, drv->Unlink(drv->ctx, 0, "blobs"));
    TEST_ASSERT_EQUAL_INT(-EPERM, drv->Unlink(drv->ctx, 0, "app:1.0.0"));
}

TEST(vfs_registry_tree, Blobs_ListsAndDescribesEachHeldBlob) {
    char names[8][70];
    char base[65];
    char top[65];
    char path[96];
    char body[160];
    int fd;
    int n;

    install("app:1", 0, "base");
    install("app:1", 1, "top");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 2, false, 0));
    sha("base", base);
    sha("top", top);

    TEST_ASSERT_EQUAL_INT(2, listDir("blobs", names, 8));
    TEST_ASSERT_EQUAL_STRING(base, names[0]);
    TEST_ASSERT_EQUAL_STRING(top, names[1]);

    snprintf(path, sizeof(path), "blobs/%s", base);
    fd = drv->Open(drv->ctx, path, VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    n = drv->Read(drv->ctx, fd, body, sizeof(body) - 1);
    TEST_ASSERT_TRUE(n > 0);
    body[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(body, "\"size\":4"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"entries\":1"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"mapped\":false"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"resident\":false"));
    TEST_ASSERT_NOT_NULL(strstr(body, "\"store\":\"mem\""));
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, fd, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
}

TEST(vfs_registry_tree, Blob_AReadServesTheBodyOfTheOpen) {
    reg_entry_t e = MakeEntry("app", "1", 0);
    wapp_t w;
    char hex[65];
    char path[96];
    char body[160];
    int fd;
    int got;
    int n;

    memset(&w, 0, sizeof(w));
    install("app:1", 0, "base");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 1, false, 0));
    sha("base", hex);
    snprintf(path, sizeof(path), "blobs/%s", hex);
    fd = drv->Open(drv->ctx, path, VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);

    got = drv->Read(drv->ctx, fd, body, 10);
    TEST_ASSERT_EQUAL_INT(10, got);
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &w));
    n = drv->Read(drv->ctx, fd, body + got, sizeof(body) - 1 - (size_t)got);
    TEST_ASSERT_TRUE(n > 0);
    body[got + n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(body, "\"mapped\":false"));
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, fd, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
    RegistryUnload(&w);
}

TEST(vfs_registry_tree, Blob_AReadPastTheEndOfAShorterBodyReturnsNothing) {
    reg_entry_t e = MakeEntry("app", "1", 0);
    wapp_t w;
    char hex[65];
    char path[96];
    char body[160];
    int fd;

    memset(&w, 0, sizeof(w));
    install("app:1", 0, "base");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 1, false, 0));
    sha("base", hex);
    snprintf(path, sizeof(path), "blobs/%s", hex);
    fd = drv->Open(drv->ctx, path, VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_TRUE(drv->Read(drv->ctx, fd, body, sizeof(body)) > 0);
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &w));
    TEST_ASSERT_EQUAL_INT(0, drv->Read(drv->ctx, fd, body, sizeof(body)));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
    RegistryUnload(&w);
}

TEST(vfs_registry_tree, Blobs_RefuseAnAbsentOrMalformedName) {
    char path[96];
    char hex[65];

    install("app:1", 0, "staged");
    sha("staged", hex);
    snprintf(path, sizeof(path), "blobs/%s", hex);
    TEST_ASSERT_EQUAL_INT(-ENOENT, drv->Open(drv->ctx, path, VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "blobs/abc", VFS_O_RDONLY));
    memset(hex, 'A', 64);
    hex[64] = '\0';
    snprintf(path, sizeof(path), "blobs/%s", hex);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, path, VFS_O_RDONLY));
}

TEST(vfs_registry_tree, Link_StagesAHeldBlobAndTheImageCommits) {
    reg_entry_t list[4];
    char base[65];
    char payload[80];
    char path[64];

    install("app:1", 0, "base");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 1, false, 0));
    sha("base", base);

    snprintf(payload, sizeof(payload), "%s\n", base);
    snprintf(path, sizeof(path), "images/app:2#0.link");
    sendLink(path, payload, 0);
    install("app:2", 1, "top");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:2", 2, false, 0));

    TEST_ASSERT_EQUAL_INT(2, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_UINT32(7, list[1].size);
}

TEST(vfs_registry_tree, Link_TakesTheDigestWithoutANewline) {
    char base[65];

    install("app:1", 0, "base");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 1, false, 0));
    sha("base", base);
    sendLink("images/app:2#0.link", base, 0);
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:2", 1, false, 0));
}

TEST(vfs_registry_tree, Link_RefusesABadPayloadOrAnAbsentBlob) {
    char base[65];
    char upper[65];
    char none[65];

    install("app:1", 0, "base");
    TEST_ASSERT_EQUAL_INT(0, RegistryCommit("app:1", 1, false, 0));
    sha("base", base);
    memcpy(upper, base, sizeof(upper));
    upper[0] = 'A';
    memset(none, '0', 64);
    none[64] = '\0';

    sendLink("images/app:2#0.link", "abc", -EINVAL);
    sendLink("images/app:2#0.link", upper, -EINVAL);
    sendLink("images/app:2#0.link", none, -ENOENT);
    TEST_ASSERT_EQUAL_INT(-ENOENT, RegistryCommit("app:2", 1, false, 0));
}

TEST(vfs_registry_tree, Link_RefusesAnOversizedPayloadAtTheWrite) {
    int fd = drv->Open(drv->ctx, "images/app:2#0.link", VFS_O_WRONLY);
    char big[80];

    memset(big, 'a', sizeof(big));
    TEST_ASSERT_TRUE(fd >= 0);
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE,
                          drv->Write(drv->ctx, fd, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
}

TEST(vfs_registry_tree, Link_AnIndexPastTheLimitIsRefusedAtTheOpen) {
    char path[64];

    snprintf(path, sizeof(path), "images/app:2#%d.link",
             REGISTRY_META_MAX_LAYERS);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, path, VFS_O_WRONLY));
}

TEST_GROUP_RUNNER(vfs_registry_tree) {
    RUN_TEST_CASE(vfs_registry_tree, Root_ListsBlobsCapacityAndImages);
    RUN_TEST_CASE(vfs_registry_tree, TheFlatPathsAreGone);
    RUN_TEST_CASE(vfs_registry_tree, TheRootAndTheDirectoriesRefuseWrites);
    RUN_TEST_CASE(vfs_registry_tree, Blobs_ListsAndDescribesEachHeldBlob);
    RUN_TEST_CASE(vfs_registry_tree, Blob_AReadServesTheBodyOfTheOpen);
    RUN_TEST_CASE(vfs_registry_tree,
                  Blob_AReadPastTheEndOfAShorterBodyReturnsNothing);
    RUN_TEST_CASE(vfs_registry_tree, Blobs_RefuseAnAbsentOrMalformedName);
    RUN_TEST_CASE(vfs_registry_tree, Link_StagesAHeldBlobAndTheImageCommits);
    RUN_TEST_CASE(vfs_registry_tree, Link_TakesTheDigestWithoutANewline);
    RUN_TEST_CASE(vfs_registry_tree, Link_RefusesABadPayloadOrAnAbsentBlob);
    RUN_TEST_CASE(vfs_registry_tree, Link_RefusesAnOversizedPayloadAtTheWrite);
    RUN_TEST_CASE(vfs_registry_tree,
                  Link_AnIndexPastTheLimitIsRefusedAtTheOpen);
}
