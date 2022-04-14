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
    wapp_data_t ctx;
    wapp_t w = { 0 };
    int ret;

    w.img = test_wasi;
    w.img_len = test_wasi_len;
    strcpy(w.cfg.console[0].name, "null");
    strcpy(w.cfg.console[1].name, "null");
    strcpy(w.cfg.console[2].name, "null");

    ctx.id = 0;
    ctx.wapp = w;

    ret = WantedWappRun(&ctx);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

TEST_GROUP_RUNNER(wanted_api)
{
    RUN_TEST_CASE(wanted_api, runSimpleWasm);
}
