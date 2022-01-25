#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

#include <vfs.h>

/***************************************/
TEST_GROUP(vfs);
/***************************************/

TEST_SETUP(vfs)
{
}

TEST_TEAR_DOWN(vfs)
{
}

TEST(vfs, runSimpleWasm)
{
    TEST_ASSERT_EQUAL_INT(0, 2);
}

TEST_GROUP_RUNNER(vfs)
{
    RUN_TEST_CASE(vfs, runSimpleWasm);
}
