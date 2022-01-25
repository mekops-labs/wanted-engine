#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <wanted.h>

#include "test_wasi.wasm.h"

/***************************************/
TEST_GROUP(general);
/***************************************/

TEST_SETUP(general)
{
}

TEST_TEAR_DOWN(general)
{
}

TEST(general, runSimpleWasm)
{
    data_t ctx;
    wapp_t w = {
        .img = test_wasi_wasm,
        .img_len = test_wasi_wasm_len
        };
    int ret;

    ctx.id = 0;
    ctx.wapp = &w;

    ret = RunWapp(&ctx);
    TEST_ASSERT_EQUAL_INT(0, ret);
}

TEST_GROUP_RUNNER(general)
{
    RUN_TEST_CASE(general, runSimpleWasm);
}
