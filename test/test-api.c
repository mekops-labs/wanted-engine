#include "unity_fixture.h"

#include <errno.h>
#include <string.h>

/***************************************/
TEST_GROUP(general);
/***************************************/

TEST_SETUP(general)
{
}

TEST_TEAR_DOWN(general)
{
}

TEST(general, someTest)
{
    TEST_ASSERT_EQUAL_INT(0, 1);
}

TEST_GROUP_RUNNER(general)
{
    RUN_TEST_CASE(general, someTest);
}
