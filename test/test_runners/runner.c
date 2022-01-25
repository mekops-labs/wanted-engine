#include "unity_fixture.h"

static void runAllTests(void)
{
    RUN_TEST_GROUP(vfs);
    RUN_TEST_GROUP(general);
}

int main(int argc, const char* argv[])
{
    return UnityMain(argc, argv, runAllTests);
}
