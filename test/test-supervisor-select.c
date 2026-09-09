/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <string.h>

#include <wanted-vfs-api.h>

/***************************************/
TEST_GROUP(supervisor_select);
/***************************************/

TEST_SETUP(supervisor_select) {}

TEST_TEAR_DOWN(supervisor_select) {}

static const wantedConfig_t *parse(const char *json) {
    TEST_ASSERT_EQUAL_INT(0, WantedParseConfig(json, strlen(json)));
    const wantedConfig_t *cfg = WantedGetConfig();
    TEST_ASSERT_NOT_NULL(cfg);
    return cfg;
}

#define SUPERVISOR_CFG(body)                                                   \
    "{\"system\":{\"privileged\":true},"                                       \
    "\"supervisor\":{\"imagePath\":\"registry:supervisor\"" body "}}"

/* The firmware's own supervisor takes over by default, so a board flashed with
 * a newer one runs it without an operator asking. */
TEST(supervisor_select, PrefersTheBundledImageByDefault) {
    TEST_ASSERT_TRUE(parse(SUPERVISOR_CFG(""))->supervisorPreferBundled);
}

TEST(supervisor_select, KeepInstalledHoldsTheInstalledImage) {
    TEST_ASSERT_FALSE(parse(SUPERVISOR_CFG(",\"keepInstalled\":true"))
                          ->supervisorPreferBundled);
}

TEST(supervisor_select, KeepInstalledFalseIsTheDefaultSpeltOut) {
    TEST_ASSERT_TRUE(parse(SUPERVISOR_CFG(",\"keepInstalled\":false"))
                         ->supervisorPreferBundled);
}

/* A config naming no supervisor at all still carries the default, so the field
 * is never left reading as "keep installed" by omission. */
TEST(supervisor_select, ADefaultSurvivesAConfigWithNoSupervisorBlock) {
    TEST_ASSERT_TRUE(
        parse("{\"system\":{\"privileged\":false}}")->supervisorPreferBundled);
}

/* A non-boolean is not an opt-out: only `true` holds the installed image. */
TEST(supervisor_select, ANonBooleanDoesNotOptOut) {
    TEST_ASSERT_TRUE(parse(SUPERVISOR_CFG(",\"keepInstalled\":\"yes\""))
                         ->supervisorPreferBundled);
}

TEST_GROUP_RUNNER(supervisor_select) {
    RUN_TEST_CASE(supervisor_select, PrefersTheBundledImageByDefault);
    RUN_TEST_CASE(supervisor_select, KeepInstalledHoldsTheInstalledImage);
    RUN_TEST_CASE(supervisor_select, KeepInstalledFalseIsTheDefaultSpeltOut);
    RUN_TEST_CASE(supervisor_select,
                  ADefaultSurvivesAConfigWithNoSupervisorBlock);
    RUN_TEST_CASE(supervisor_select, ANonBooleanDoesNotOptOut);
}
