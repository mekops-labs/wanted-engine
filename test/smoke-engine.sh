#!/bin/bash
# Engine smoke test for the production supervisor wapp.
#
# Boots wanted-cli with the supervisor TAR image and asserts the engine
# loads it without crashing. Specifically:
#   1. The CLI exits with a non-crash code — 0 (supervisor proc_exit'd and
#      PlatformWappLoop returned because all wapps drained) or 124 (timeout
#      hit while supervisor was still running / being respawned).
#   2. No fatal-load markers appear in the output (only meaningful when the
#      binary is built with WANTED_DEBUG_TRACES=ON; if traces are off the
#      engine is silent on success and the exit-code check is the only gate).
#
# Output checks are deliberately loose because the production stdio teardown
# closes host fd 1, dropping any "All wapps ended" message the CLI tries to
# print after WantedStart returns. The real correctness signal is the absence
# of a crash plus the absence of fatal markers.
#
# When run with a Sheriff M0+ supervisor that writes its clock-quality byte
# (and traces are off), the byte will be the only thing on stdout — we report
# it but do not gate on it because the checked-in supervisor TAR may predate
# that change.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$PROJECT_ROOT"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./test/smoke-engine-config.json}

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli binary not found at $WANTED"
    exit 1
fi
if [ ! -f "$CONFIG" ]; then
    echo "FAIL: smoke config not found at $CONFIG"
    exit 1
fi

OUT=$(mktemp)
trap "rm -f $OUT" EXIT

stdbuf -o0 -e0 timeout 5 "$WANTED" "$CONFIG" >"$OUT" 2>&1
RC=$?

# Trace-based fatal markers (only present with WANTED_DEBUG_TRACES=ON).
if grep -qE 'Fatal:|fail to link|missing import|undefined symbol|fail to load wasm|can.t open wapp|can.t initialize tarfs' "$OUT"; then
    echo "FAIL: fatal marker in engine output"
    cat "$OUT"
    exit 1
fi

# Crash signals manifest as exit codes >= 128 (e.g. 139 for SIGSEGV).
# Acceptable: 0 (clean shutdown) and 124 (timeout — supervisor still running).
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
    echo "FAIL: unexpected exit code $RC (likely a crash)"
    cat "$OUT"
    exit 1
fi

echo "PASS: supervisor instantiated cleanly (rc=$RC)"

if LC_ALL=C grep -qE '[0-3]' "$OUT"; then
    echo "INFO: at least one '0'..'3' byte present on stdout (possible clock-quality byte)"
fi
exit 0
