/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <dummy-fs.h>
#include <vfs.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

/***************************************/
TEST_GROUP(wanted_vfs_api);
/***************************************/

TEST_SETUP(wanted_vfs_api) {}

TEST_TEAR_DOWN(wanted_vfs_api) {}

TEST(wanted_vfs_api, WantedParseCtrlActionTest) {
    int ret;
    const char *buf =
#include "ctrl-action.json.h"
        ;

    char appName[WAPP_MAX_NAME_LEN];
    wapp_action_t act = WAPP_STOP + 1;
    wapp_config_t cfg;

    ret = WantedParseCtrlActionJson(buf, strlen(buf), appName, &act, &cfg);
    TEST_ASSERT_EQUAL(0, ret);

    TEST_ASSERT_EQUAL_STRING("app1", appName);
    TEST_ASSERT_EQUAL(WAPP_START, act);
    TEST_ASSERT_EQUAL_STRING("app1img", cfg.image);

    /* drivers[]: device singletons, name only. */
    TEST_ASSERT_EQUAL(2, cfg.driversCnt);
    TEST_ASSERT_EQUAL_STRING("wanted", cfg.drivers[0].name);
    TEST_ASSERT_EQUAL_STRING("gpio", cfg.drivers[1].name);

    /* mounts[]: file/backend drivers at an arbitrary path. */
    TEST_ASSERT_EQUAL(2, cfg.mountsCnt);
    TEST_ASSERT_EQUAL_STRING("config", cfg.mounts[0].name);
    TEST_ASSERT_EQUAL_STRING("/etc/config", cfg.mounts[0].path);
    TEST_ASSERT_EQUAL_STRING("platform", cfg.mounts[1].name);
    TEST_ASSERT_EQUAL_STRING("/mnt", cfg.mounts[1].path);

    /* sockets[]: the transport spec is carried in options (the "address"
     * field); the name is the /net node label. */
    TEST_ASSERT_EQUAL(1, cfg.socketsCnt);
    TEST_ASSERT_EQUAL_STRING("uplink", cfg.sockets[0].name);
    TEST_ASSERT_EQUAL_STRING("tcp://127.0.0.1:8888", cfg.sockets[0].options);

    /* args occupy argv[1..]; argv[0] is the wapp name, set by the engine. */
    TEST_ASSERT_EQUAL(2, cfg.argsCnt);
    TEST_ASSERT_EQUAL_STRING("--verbose", cfg.args[0]);
    TEST_ASSERT_EQUAL_STRING("--port", cfg.args[1]);

    /* envs are POSIX "KEY=VALUE" strings. */
    TEST_ASSERT_EQUAL(2, cfg.envsCnt);
    TEST_ASSERT_EQUAL_STRING("TZ=UTC", cfg.envs[0]);
    TEST_ASSERT_EQUAL_STRING("LANG=C", cfg.envs[1]);
}

/* The decomposed config node carries a bare params object (no envelope). */
TEST(wanted_vfs_api, WantedParseWappConfigArgsEnvs) {
    const char *cfg_json = "{\"args\":[\"one\",\"two\",\"three\"],"
                           "\"envs\":[\"A=1\",\"B=2\"]}";
    wapp_config_t cfg;

    int ret = WantedParseWappConfigJson(cfg_json, strlen(cfg_json), &cfg);
    TEST_ASSERT_EQUAL(0, ret);

    TEST_ASSERT_EQUAL(3, cfg.argsCnt);
    TEST_ASSERT_EQUAL_STRING("one", cfg.args[0]);
    TEST_ASSERT_EQUAL_STRING("three", cfg.args[2]);
    TEST_ASSERT_EQUAL(2, cfg.envsCnt);
    TEST_ASSERT_EQUAL_STRING("A=1", cfg.envs[0]);
}

/* The config node parses the optional "image" field — the image an instance
 * runs, decoupled from its instance name. */
TEST(wanted_vfs_api, WantedParseWappConfigImage) {
    const char *cfg_json = "{\"image\":\"duplex\",\"envs\":[\"ROLE=reader\"]}";
    wapp_config_t cfg;

    int ret = WantedParseWappConfigJson(cfg_json, strlen(cfg_json), &cfg);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("duplex", cfg.image);

    /* Omitting "image" leaves it empty (the launch path defaults it to the
     * instance name). */
    const char *no_image = "{\"args\":[\"x\"]}";
    ret = WantedParseWappConfigJson(no_image, strlen(no_image), &cfg);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("", cfg.image);
}

/* A pinned image reference "<name>:<tag>" round-trips through the config — the
 * tag is opaque and resolved exactly at launch. */
TEST(wanted_vfs_api, WantedParseWappConfigImagePinnedTag) {
    const char *cfg_json = "{\"image\":\"duplex:stable\"}";
    wapp_config_t cfg;

    int ret = WantedParseWappConfigJson(cfg_json, strlen(cfg_json), &cfg);
    TEST_ASSERT_EQUAL(0, ret);
    TEST_ASSERT_EQUAL_STRING("duplex:stable", cfg.image);
}

/* A launch config naming a driver no table offers fails the install with
 * -ENODEV. The engine rejects the launch rather than silently no-op'ing, and
 * survives. */
TEST(wanted_vfs_api, InstallUnavailableDriverReturnsEnodev) {
    vfs_ctx_t c = VfsInit();
    TEST_ASSERT_NOT_NULL(c);

    wapp_t *w = WantedMalloc(sizeof(wapp_t));
    TEST_ASSERT_NOT_NULL(w);
    memset(w, 0, sizeof(*w));

    int ret = WantedInstallDriver(c, w, "nosuchdriver", "/dev/nosuch", NULL);
    TEST_ASSERT_EQUAL_INT(-ENODEV, ret);

    WantedFree(w);
    VfsDestroy(&c);
}

/* The core drivers are always resolvable; WantedListDrivers reports them so a
 * supervisor can read available capability off /proc/wanted. */
TEST(wanted_vfs_api, ListDriversReportsCoreDrivers) {
    char buf[128];
    int n = WantedListDrivers(buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_NOT_NULL(strstr(buf, "wanted"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "null"));
    /* gpio is a core driver: its tree and grant grammar are the same on every
     * target, and only the line behind it is per-platform. */
    TEST_ASSERT_NOT_NULL(strstr(buf, "gpio"));
    /* The dummy platform contributes no platform drivers of its own. */
    TEST_ASSERT_NULL(strstr(buf, "wifi"));
}

/* ── launch-config overlay ──────────────────────────────────────────────── */

#define OVERLAY_DIR "/var/lib/sheriff"
#define OVERLAY_PATH OVERLAY_DIR "/config-overlay.json"

/* A config granting the storage-root mount and no sockets: what an
 * unprovisioned device boots with. */
static void _OverlayBaseCfg(wapp_config_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strcpy(cfg->mounts[0].name, "platform");
    strcpy(cfg->mounts[0].path, OVERLAY_DIR);
    cfg->mountsCnt = 1;
}

TEST(wanted_vfs_api, OverlaySuppliesAddressesTheConfigLacks) {
    wapp_config_t cfg;
    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(OVERLAY_PATH,
                      "{\"manager\":\"tcps://mgr.example:8443\","
                      "\"registry\":\"tcps://reg.example:5000\"}");

    TEST_ASSERT_EQUAL_INT(0, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_size_t(2, cfg.socketsCnt);
    TEST_ASSERT_EQUAL_STRING("manager", cfg.sockets[0].name);
    TEST_ASSERT_EQUAL_STRING("tcps://mgr.example:8443", cfg.sockets[0].options);
    TEST_ASSERT_EQUAL_STRING("registry", cfg.sockets[1].name);
}

/* An absent overlay is the ordinary case, not a failure: an unprovisioned
 * device, and every device provisioned before the mechanism existed. */
TEST(wanted_vfs_api, AbsentOverlayIsNotAnError) {
    wapp_config_t cfg;
    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(NULL, NULL);

    TEST_ASSERT_EQUAL_INT(0, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_size_t(0, cfg.socketsCnt);
}

/* An address already in the launch config was pinned by hand — on OpenWRT, by
 * UCI rendered into it before the engine started — and outranks the blob's. */
TEST(wanted_vfs_api, ConfiguredAddressOutranksTheOverlay) {
    wapp_config_t cfg;
    _OverlayBaseCfg(&cfg);
    strcpy(cfg.sockets[0].name, "manager");
    strcpy(cfg.sockets[0].options, "tcp://pinned.example:9000");
    cfg.socketsCnt = 1;
    DummySmallFileSet(OVERLAY_PATH,
                      "{\"manager\":\"tcps://mgr.example:8443\","
                      "\"registry\":\"tcps://reg.example:5000\"}");

    TEST_ASSERT_EQUAL_INT(0, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_STRING("tcp://pinned.example:9000",
                             cfg.sockets[0].options);
    /* The address it does not pin still arrives. */
    TEST_ASSERT_EQUAL_size_t(2, cfg.socketsCnt);
    TEST_ASSERT_EQUAL_STRING("registry", cfg.sockets[1].name);
}

/* Applying twice must converge: a reload re-merges the same overlay onto
 * grants that already carry it. */
TEST(wanted_vfs_api, MergingTwiceAddsNothingTheSecondTime) {
    wapp_config_t cfg;
    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(OVERLAY_PATH,
                      "{\"manager\":\"tcps://mgr.example:8443\"}");

    TEST_ASSERT_EQUAL_INT(0, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_INT(0, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_size_t(1, cfg.socketsCnt);
}

/* The overlay is addresses and nothing else. A silently ignored field would be
 * a grant the operator believes was applied. */
TEST(wanted_vfs_api, OverlayRefusesAFieldThatIsNotAnAddress) {
    wapp_config_t cfg;
    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(OVERLAY_PATH, "{\"manager\":\"tcps://mgr.example:8443\","
                                    "\"device_id\":\"pico-01\"}");

    TEST_ASSERT_EQUAL_INT(-EINVAL, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    /* Refused whole: the address beside it is not applied either. */
    TEST_ASSERT_EQUAL_size_t(0, cfg.socketsCnt);
}

TEST(wanted_vfs_api, OverlayRefusesMalformedDocuments) {
    wapp_config_t cfg;

    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(OVERLAY_PATH, "not json at all");
    TEST_ASSERT_EQUAL_INT(-EINVAL, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));

    _OverlayBaseCfg(&cfg);
    DummySmallFileSet(OVERLAY_PATH, "{\"manager\":\"\"}");
    TEST_ASSERT_EQUAL_INT(-EINVAL, WantedMergeConfigOverlay(&cfg, OVERLAY_DIR));
    TEST_ASSERT_EQUAL_size_t(0, cfg.socketsCnt);
}

TEST_GROUP_RUNNER(wanted_vfs_api) {
    RUN_TEST_CASE(wanted_vfs_api, WantedParseCtrlActionTest);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseWappConfigArgsEnvs);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseWappConfigImage);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseWappConfigImagePinnedTag);
    RUN_TEST_CASE(wanted_vfs_api, InstallUnavailableDriverReturnsEnodev);
    RUN_TEST_CASE(wanted_vfs_api, ListDriversReportsCoreDrivers);
    RUN_TEST_CASE(wanted_vfs_api, OverlaySuppliesAddressesTheConfigLacks);
    RUN_TEST_CASE(wanted_vfs_api, AbsentOverlayIsNotAnError);
    RUN_TEST_CASE(wanted_vfs_api, ConfiguredAddressOutranksTheOverlay);
    RUN_TEST_CASE(wanted_vfs_api, MergingTwiceAddsNothingTheSecondTime);
    RUN_TEST_CASE(wanted_vfs_api, OverlayRefusesAFieldThatIsNotAnAddress);
    RUN_TEST_CASE(wanted_vfs_api, OverlayRefusesMalformedDocuments);
}
