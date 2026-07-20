#!/bin/bash
# Build the devcheck wapp, run it as the engine supervisor with the offload
# devices granted, and assert its TAP: the sha256/ed25519/inflate wasm ->
# WASI -> VFS -> driver round trip. devcheck powers the engine off after one
# pass. Ed25519 is a SKIP (still "ok") when the build has no crypto backend.
#
# Runs inside the build container (toolchain + wanted-cli on PATH/built), the
# same way test/selftest.sh is invoked.
#
# Usage: test/devcheck.sh [wanted-cli]
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"
# shellcheck source=test/lib-wapp.sh
. "$SCRIPT_DIR/lib-wapp.sh"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=./test/devcheck-config.json
SUP_DIR=./wasm/supervisor/devcheck

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli not found at $WANTED (run 'make build')"
    exit 1
fi

# Build the wapp and package it as a supervisor image (a ustar of app.wasm).
wapp_build devcheck
mkdir -p "$SUP_DIR"
cp wapps/devcheck/devcheck.wasm "$SUP_DIR/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$SUP_DIR" -cf "$SUP_DIR/supervisor.tar" app.wasm

out=$("$WANTED" "$CONFIG" 2>&1)
echo "$out"

# Pass iff the plan ran and nothing reported "not ok".
if echo "$out" | grep -q '^1\.\.3' && ! echo "$out" | grep -q '^not ok'; then
    echo "PASS: devcheck round trips ok"
    exit 0
fi
echo "FAIL: devcheck reported failures (or did not run)"
exit 1
