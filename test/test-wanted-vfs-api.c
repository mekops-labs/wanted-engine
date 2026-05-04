#include "unity_fixture.h"

#include <stdint.h>
#include <string.h>

#include <wanted-vfs-api.h>

#include <tiny-json.h>

/*
int WantedSetConfig(wantedConfig_t cfg);
int WantedGetConfigJson(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedReadState(uint8_t *buf, size_t bufLen);
*/

/***************************************/
TEST_GROUP(wanted_vfs_api);
/***************************************/

TEST_SETUP(wanted_vfs_api) {}

TEST_TEAR_DOWN(wanted_vfs_api) {}

extern void WantedSetConfig(wantedConfig_t cfg);

TEST(wanted_vfs_api, WantedGetConfigTest) {
    int ret;
    uint8_t buf[100];
    json_t m[32];
    wantedConfig_t cfg = {
        .nWapps = 3,
        .wappsToRun =
            {
                "aa",
                "bb",
                "cc",
            },
    };

    WantedSetConfig(cfg);

    ret = WantedGetConfigJson(buf, 100);
    TEST_ASSERT_EQUAL(53, ret);

    /* check that the json is valid */
    json_t const *json = json_create((char *)buf, m, sizeof m / sizeof *m);
    TEST_ASSERT_NOT_NULL(json);

    json_t const *wapps = json_getProperty(json, "wapps");
    if (!wapps || JSON_ARRAY != json_getType(wapps)) {
        TEST_FAIL_MESSAGE("Error, the wapps not found.");
    }

    json_t const *wapp;
    int i;
    for (i = 0, wapp = json_getChild(wapps); wapp != 0 && i < cfg.nWapps;
         wapp = json_getSibling(wapp), i++) {
        TEST_ASSERT_EQUAL(JSON_OBJ, json_getType(wapp));
        const char *name = json_getPropertyValue(wapp, "name");
        TEST_ASSERT_NOT_NULL(name);
        TEST_ASSERT_EQUAL_STRING(cfg.wappsToRun[i], name);
    }
}

TEST(wanted_vfs_api, WantedReadRegistryTest) {
    int ret;
    uint8_t buf[100];
    json_t m[32];

    ret = WantedReadRegistry(buf, 100);
    // TEST_ASSERT_EQUAL(26, ret);

    return;
    /* check that the json is valid */
    json_t const *json = json_create((char *)buf, m, sizeof m / sizeof *m);
    TEST_ASSERT_NOT_NULL(json);

    json_t const *wapps = json_getProperty(json, "wapps");
    if (!wapps || JSON_ARRAY != json_getType(wapps)) {
        TEST_FAIL_MESSAGE("Error, the wapps not found.");
    }
}

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
    TEST_ASSERT_EQUAL(5, cfg.driversCnt);
}

TEST_GROUP_RUNNER(wanted_vfs_api) {
    RUN_TEST_CASE(wanted_vfs_api, WantedGetConfigTest);
    RUN_TEST_CASE(wanted_vfs_api, WantedReadRegistryTest);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseCtrlActionTest);
}
