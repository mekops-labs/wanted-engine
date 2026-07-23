#!/bin/bash
# Wraps a single command as a one-testcase JUnit report, for CI jobs (the
# NuttX sim lanes) that run exactly one check.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=test/lib-junit.sh
. "$SCRIPT_DIR/lib-junit.sh"

if [ "$#" -lt 4 ]; then
    echo "usage: $0 <report-file> <suite-name> <testcase-name> -- <command...>" >&2
    exit 2
fi

REPORT=$1
SUITE=$2
NAME=$3
shift 3
[ "${1:-}" = "--" ] && shift

junit_run "$NAME" -- "$@"
rc=$?
junit_write "$REPORT" "$SUITE"
exit "$rc"
