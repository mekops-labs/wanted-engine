#!/bin/sh

[ -z "$1" ] && {
	echo Please provide output file!
	exit 1
}

out=$1
shift

# Scan only the sources this build compiles, so the runner never calls a group
# that was dropped with its driver.
[ "$#" -gt 0 ] || set -- test-*.c

test_groups=$(sed -n -e '/TEST_GROUP_RUNNER/s/TEST_GROUP_RUNNER(\([^)]*\)).*/\1/p' "$@")

set -- "$out"

cat >$1 <<EOF
#include "unity_fixture.h"

static void runAllTests(void)
{
EOF

for i in $test_groups; do
	echo "    RUN_TEST_GROUP($i);" >>$1
done

cat >>$1 <<EOF
}

int main(int argc, const char* argv[])
{
    return UnityMain(argc, argv, runAllTests);
}
EOF
