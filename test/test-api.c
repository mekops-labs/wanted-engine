#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <wanted-api.h>

#include "external_symbols.h"

/***************************************/
TEST_GROUP(wanted_api);
/***************************************/

TEST_SETUP(wanted_api)
{
}

TEST_TEAR_DOWN(wanted_api)
{
}

TEST(wanted_api, runSimpleWasm)
{
    data_t ctx;
    wapp_t w = {
        .img = test_wasi_romfs,
        .img_len = test_wasi_romfs_len
    };
    int ret;

    ctx.id = 0;
    ctx.wapp = w;

    ret = RunWapp(&ctx);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

TEST_GROUP_RUNNER(wanted_api)
{
    RUN_TEST_CASE(wanted_api, runSimpleWasm);
}
