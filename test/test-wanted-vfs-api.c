/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <wanted-vfs-api.h>

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
    TEST_ASSERT_EQUAL(5, cfg.driversCnt);

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
    const char *cfg_json =
        "{\"args\":[\"one\",\"two\",\"three\"],"
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

/* ParseVersionString maps a registry version field "a.b.c-d" onto the packed
 * wapp_version_t, zero-filling missing trailing fields and rejecting malformed
 * input. */
TEST(wanted_vfs_api, ParseVersionStringParsesAndZeroFills) {
    wapp_version_t v;

    TEST_ASSERT_EQUAL(0, ParseVersionString("1.2.3-4", &v));
    TEST_ASSERT_EQUAL_UINT8(1, v.major);
    TEST_ASSERT_EQUAL_UINT8(2, v.minor);
    TEST_ASSERT_EQUAL_UINT8(3, v.patch);
    TEST_ASSERT_EQUAL_UINT8(4, v.package);

    /* Trailing fields are optional and zero-fill. */
    TEST_ASSERT_EQUAL(0, ParseVersionString("2.5", &v));
    TEST_ASSERT_EQUAL_UINT8(2, v.major);
    TEST_ASSERT_EQUAL_UINT8(5, v.minor);
    TEST_ASSERT_EQUAL_UINT8(0, v.patch);
    TEST_ASSERT_EQUAL_UINT8(0, v.package);

    /* An empty string is all zeros. */
    TEST_ASSERT_EQUAL(0, ParseVersionString("", &v));
    TEST_ASSERT_EQUAL_UINT8(0, v.major);

    /* Non-numeric and out-of-range fields are rejected. */
    TEST_ASSERT_EQUAL(-EINVAL, ParseVersionString("x.2.3", &v));
    TEST_ASSERT_EQUAL(-EINVAL, ParseVersionString("999.0.0", &v));
}

TEST_GROUP_RUNNER(wanted_vfs_api) {
    RUN_TEST_CASE(wanted_vfs_api, WantedParseCtrlActionTest);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseWappConfigArgsEnvs);
    RUN_TEST_CASE(wanted_vfs_api, WantedParseWappConfigImage);
    RUN_TEST_CASE(wanted_vfs_api, ParseVersionStringParsesAndZeroFills);
}
