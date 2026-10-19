/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <registry-mem.h>
#include <registry.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include "dummy-fs.h"
#include "test-utils.h"

/* vfs_registry_layers — the layer and commit routes of the "reg" node. */

extern const vfs_driver_t WantedRegistryDriver;

TEST_GROUP(vfs_registry_layers);

static const vfs_driver_t *drv;
static wapp_t wapp;

static void closeAll(void) {
    for (int i = 0; i < 16; i++)
        drv->Close(drv->ctx, i);
}

TEST_SETUP(vfs_registry_layers) {
    DummyRegistryReset();
    DummyClockReset();
    WantedSetClockQuality(WANTED_CLOCK_UNCALIBRATED);
    drv = &WantedRegistryDriver;
    memset(&wapp, 0, sizeof(wapp));
    closeAll();
}

TEST_TEAR_DOWN(vfs_registry_layers) {
    closeAll();
    DummyRegistryReset();
}

static int openWrite(const char *path) {
    int fd = drv->Open(drv->ctx, path, VFS_O_WRONLY);

    TEST_ASSERT_TRUE_MESSAGE(fd >= 0, path);
    return fd;
}

static void sendLayer(const char *path, const char *bytes) {
    int fd = openWrite(path);

    TEST_ASSERT_EQUAL_INT((int)strlen(bytes),
                          drv->Write(drv->ctx, fd, bytes, strlen(bytes)));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
}

static int commit(const char *ref, const uint8_t *payload, size_t n) {
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "%s#layers", ref);
    fd = openWrite(path);
    if (n > 0) {
        int w = drv->Write(drv->ctx, fd, payload, n);

        if (w < 0) {
            drv->Close(drv->ctx, fd);
            return w;
        }
    }
    return drv->Close(drv->ctx, fd);
}

static int commitCount(const char *ref, uint8_t count) {
    return commit(ref, &count, 1);
}

TEST(vfs_registry_layers, LayersThenCommit_PublishTheEntry) {
    reg_entry_t list[4];

    sendLayer("app:1.0.0#0", "base");
    sendLayer("app:1.0.0#1", "top");
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));

    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 2));
    TEST_ASSERT_EQUAL_INT(1, RegistryRead(list, 4));
    TEST_ASSERT_EQUAL_STRING("app", list[0].name);
    TEST_ASSERT_EQUAL_UINT32(7, list[0].size);
}

TEST(vfs_registry_layers, Layers_ArriveInAnyOrder) {
    registry_meta_t meta;
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);

    sendLayer("app:1.0.0#1", "top");
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 2));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_UINT32(4, meta.layerLen[0]);
    TEST_ASSERT_EQUAL_UINT32(3, meta.layerLen[1]);
}

TEST(vfs_registry_layers, Commit_WithAMissingLayer_FailsAtClose) {
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(-ENOENT, commitCount("app:1.0.0", 2));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
}

TEST(vfs_registry_layers, Commit_CarriesTheBuildTime) {
    static const uint8_t payload[9] = {1, 0, 0, 0, 0, 0x65, 0x53, 0xf1, 0x00};
    registry_meta_t meta;
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);

    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(0, commit("app:1.0.0", payload, sizeof(payload)));
    TEST_ASSERT_EQUAL_INT(0, RegistryMetaRead(&e, &meta));
    TEST_ASSERT_EQUAL_INT64(0x006553f100LL, meta.created);
}

TEST(vfs_registry_layers, Commit_PayloadOfAnyOtherLength_IsRefused) {
    static const uint8_t two[2] = {1, 1};
    static const uint8_t ten[10] = {1};

    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(-EINVAL, commit("app:1.0.0", two, sizeof(two)));
    TEST_ASSERT_EQUAL_INT(-EMSGSIZE, commit("app:1.0.0", ten, sizeof(ten)));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
}

TEST(vfs_registry_layers, Commit_OfNoBytes_IsANoOp) {
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(0, commit("app:1.0.0", NULL, 0));
    TEST_ASSERT_EQUAL_INT(0, RegistryRead(NULL, 0));
}

TEST(vfs_registry_layers, Commit_CountOfZeroOrAboveTheLimit_IsRefused) {
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(-EINVAL, commitCount("app:1.0.0", 0));
    TEST_ASSERT_EQUAL_INT(
        -E2BIG, commitCount("app:1.0.0", REGISTRY_META_MAX_LAYERS + 1));
}

TEST(vfs_registry_layers, LayerIndex_AtTheLimit_IsRefusedAtOpen) {
    char path[64];

    snprintf(path, sizeof(path), "app:1.0.0#%d", REGISTRY_META_MAX_LAYERS);
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, path, VFS_O_WRONLY));
    snprintf(path, sizeof(path), "app:1.0.0#%d", REGISTRY_META_MAX_LAYERS - 1);
    TEST_ASSERT_TRUE(drv->Open(drv->ctx, path, VFS_O_WRONLY) >= 0);
}

TEST(vfs_registry_layers, RouteGrammar_RefusesWhatIsNotARoute) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, drv->Open(drv->ctx, "app:1#", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:1#x", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:1#001", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app:1#0.sig", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "ap#p:1#0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          drv->Open(drv->ctx, "app 1:1#0", VFS_O_WRONLY));
    TEST_ASSERT_EQUAL_INT(-ENAMETOOLONG,
                          drv->Open(drv->ctx, "#0", VFS_O_WRONLY));
}

TEST(vfs_registry_layers, RouteIsNotAnEntryToRead) {
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 1));
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          drv->Open(drv->ctx, "app:1.0.0#0", VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(
        -ENOENT, drv->Open(drv->ctx, "app:1.0.0#layers", VFS_O_RDONLY));
}

TEST(vfs_registry_layers, TwoLayerWritesAtOnce_TheSecondGetsEbusy) {
    int first = openWrite("app:1.0.0#0");
    int second = openWrite("app:1.0.0#1");

    TEST_ASSERT_EQUAL_INT(4, drv->Write(drv->ctx, first, "base", 4));
    TEST_ASSERT_EQUAL_INT(-EBUSY, drv->Write(drv->ctx, second, "top", 3));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, second));
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, first));

    sendLayer("app:1.0.0#1", "top");
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 2));
}

TEST(vfs_registry_layers, LayerWrite_NeverChangesARunningImage) {
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);

    sendLayer("app:1.0.0#0", "old-image");
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 1));
    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));

    sendLayer("app:1.0.0#0", "new-image");
    TEST_ASSERT_EQUAL_MEMORY("old-image", wapp.layers[0], 9);
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 1));
    TEST_ASSERT_EQUAL_MEMORY("old-image", wapp.layers[0], 9);
    RegistryUnload(&wapp);
}

TEST(vfs_registry_layers, Descriptor_ReportsLayersTimesAndUse) {
    static const uint8_t payload[9] = {2, 0, 0, 0, 0, 0x65, 0x53, 0xf1, 0x00};
    reg_entry_t e = MakeEntry("app", "1.0.0", 0);
    char buf[512];
    int fd;
    int n;

    sendLayer("app:1.0.0#0", "base");
    sendLayer("app:1.0.0#1", "top");
    TEST_ASSERT_EQUAL_INT(0, commit("app:1.0.0", payload, sizeof(payload)));

    fd = drv->Open(drv->ctx, "app:1.0.0", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_TRUE(n > 0);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"layers\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"clock_set\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"installed\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"created\":1700000000"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mapped\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"available\":true"));
    drv->Close(drv->ctx, fd);

    TEST_ASSERT_EQUAL_INT(0, RegistryLoad(&e, &wapp));
    fd = drv->Open(drv->ctx, "app:1.0.0", VFS_O_RDONLY);
    n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mapped\":true"));
    RegistryUnload(&wapp);
}

TEST(vfs_registry_layers, Descriptor_ReportsTheInstallTimeOfASetClock) {
    char buf[512];
    int fd;
    int n;

    TEST_ASSERT_EQUAL_INT(0,
                          PlatformClockSetTime(PLAT_CLOCKID_REALTIME,
                                               1700000000ULL * 1000000000ULL));
    WantedSetClockQuality(WANTED_CLOCK_HARDWARE_RTC);
    sendLayer("app:1.0.0#0", "base");
    TEST_ASSERT_EQUAL_INT(0, commitCount("app:1.0.0", 1));

    fd = drv->Open(drv->ctx, "app:1.0.0", VFS_O_RDONLY);
    n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    buf[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"clock_set\":true"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"installed\":17000000"));
}

TEST_GROUP_RUNNER(vfs_registry_layers) {
    RUN_TEST_CASE(vfs_registry_layers, LayersThenCommit_PublishTheEntry);
    RUN_TEST_CASE(vfs_registry_layers, Layers_ArriveInAnyOrder);
    RUN_TEST_CASE(vfs_registry_layers, Commit_WithAMissingLayer_FailsAtClose);
    RUN_TEST_CASE(vfs_registry_layers, Commit_CarriesTheBuildTime);
    RUN_TEST_CASE(vfs_registry_layers,
                  Commit_PayloadOfAnyOtherLength_IsRefused);
    RUN_TEST_CASE(vfs_registry_layers, Commit_OfNoBytes_IsANoOp);
    RUN_TEST_CASE(vfs_registry_layers,
                  Commit_CountOfZeroOrAboveTheLimit_IsRefused);
    RUN_TEST_CASE(vfs_registry_layers, LayerIndex_AtTheLimit_IsRefusedAtOpen);
    RUN_TEST_CASE(vfs_registry_layers, RouteGrammar_RefusesWhatIsNotARoute);
    RUN_TEST_CASE(vfs_registry_layers, RouteIsNotAnEntryToRead);
    RUN_TEST_CASE(vfs_registry_layers, TwoLayerWritesAtOnce_TheSecondGetsEbusy);
    RUN_TEST_CASE(vfs_registry_layers, LayerWrite_NeverChangesARunningImage);
    RUN_TEST_CASE(vfs_registry_layers, Descriptor_ReportsLayersTimesAndUse);
    RUN_TEST_CASE(vfs_registry_layers,
                  Descriptor_ReportsTheInstallTimeOfASetClock);
}
