#!/bin/bash
# Runs the Linux integration suite as one JUnit-reported unit. Every check
# runs regardless of earlier failures, so one bad check doesn't hide the rest.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"
# shellcheck source=test/lib-junit.sh
. "$SCRIPT_DIR/lib-junit.sh"

BUILD_DIR=${BUILD_DIR:-build}
REPORT="$BUILD_DIR/integration-junit.xml"

junit_run smoke-engine -- just smoke-engine
junit_run selftest -- just selftest
junit_run syscontrol -- just syscontrol
junit_run live-update -- just live-update

junit_write "$REPORT" integration
echo "==> integration report: $REPORT"
junit_exit_code
