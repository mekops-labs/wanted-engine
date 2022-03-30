#include "unity_fixture.h"

#include <stdint.h>
#include <errno.h>
#include <string.h>

#include <stdio.h>

//#include <wanted-api.h>
#include <wanted-vfs-api.h>

#include <tiny-json.h>

/*
int WantedSetConfig(wantedConfig_t cfg);
int WantedGetConfig(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedReadState(uint8_t *buf, size_t bufLen);
*/

/***************************************/
TEST_GROUP(wanted_vfs_api);
/***************************************/

TEST_SETUP(wanted_vfs_api)
{
}

TEST_TEAR_DOWN(wanted_vfs_api)
{
}

TEST(wanted_vfs_api, WantedGetConfigTest)
{
    int ret;
    uint8_t buf[100];
    json_t m[32];

    ret = WantedGetConfig(buf, 100);

    TEST_ASSERT_EQUAL(13, ret);

    json_t const* json = json_create(buf, m, sizeof m / sizeof *m);

    TEST_ASSERT_NOT_NULL(json);

}

TEST_GROUP_RUNNER(wanted_vfs_api)
{
    RUN_TEST_CASE(wanted_vfs_api, WantedGetConfigTest);
}
