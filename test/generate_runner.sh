#!/bin/sh

[ -z "$1" ] && { echo Please provide output file!; exit 1; }

test_groups=`sed -n -e '/TEST_GROUP_RUNNER/s/TEST_GROUP_RUNNER(\(.*\))/\1/p' test-*.c`

cat > $1 <<EOF
#include "unity_fixture.h"

static void runAllTests(void)
{
EOF

for i in $test_groups; do
    echo "    RUN_TEST_GROUP($i);" >> $1
done

cat >> $1 <<EOF
}

int main(int argc, const char* argv[])
{
    return UnityMain(argc, argv, runAllTests);
}
EOF
