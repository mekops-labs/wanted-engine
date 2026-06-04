#!/bin/bash
# Multi-wapp smoke test: drive the wsh debug supervisor to launch a second
# wapp and assert the engine runs both concurrently.
#
# Flow:
#   1. Build + package the `hello` sample wapp (wapps/hello/) into a registry
#      image at REGISTRY_ROOT (./wapps), the directory the engine's Linux
#      registry scans for `<name>:<version>.wapp` archives.
#   2. Boot wanted-cli with the wsh supervisor and feed it three shell lines:
#        - write a launch config (console = platform) to the control plane so
#          the wapp gets a working stdio when started;
#        - `start hello`  — create-and-launch via /dev/wanted/ctl;
#        - `status`       — enumerate /dev/wanted/wapps and print each state.
#   3. Assert the `status` snapshot — taken while the supervisor is still the
#      live foreground wapp — shows BOTH `supervisor` and `hello` running. That
#      is the concurrency signal: two isolated wapps live at the same instant.
#
# Why status and not the wapp's own stdout: the production stdio teardown
# closes host fd 1 when the first wapp (the supervisor) exits on stdin EOF, so
# anything the launched wapp prints afterwards is dropped. The control-plane
# state read happens before that and is the reliable observation point.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$PROJECT_ROOT"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./docs/example_config_smoke.json}

# Sample wapp under test. Keep WAPP_VERSION in sync with wapps/hello/manifest.json
# (version array + package => MAJOR.MINOR.PATCH-PACKAGE); the registry resolves
# `start hello` to ./wapps/hello:<version>.wapp.
WAPP_NAME=hello
WAPP_VERSION=0.0.1-1
WAPP_SRC_DIR=wapps/$WAPP_NAME
REGISTRY_ROOT=./wapps
WAPP_IMAGE=$REGISTRY_ROOT/$WAPP_NAME:$WAPP_VERSION.wapp

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli binary not found at $WANTED"
    exit 1
fi
if [ ! -f "$CONFIG" ]; then
    echo "FAIL: smoke config not found at $CONFIG"
    exit 1
fi

# Build the wapp wasm (idempotent) and package it as an OCI-style ustar image.
if ! make -C "$WAPP_SRC_DIR" >/dev/null 2>&1; then
    echo "FAIL: could not build $WAPP_SRC_DIR (wasi-sdk toolchain required)"
    exit 1
fi

STAGE=$(mktemp -d)
cleanup() { rm -rf "$STAGE"; rm -f "$WAPP_IMAGE"; }
trap cleanup EXIT

cp "$WAPP_SRC_DIR/$WAPP_NAME.wasm" "$STAGE/app.wasm"
cp "$WAPP_SRC_DIR/manifest.json" "$STAGE/manifest.json"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$STAGE" -cf "$WAPP_IMAGE" app.wasm manifest.json

# Launch config granted to the wapp: a null stdin and platform-backed
# stdout/stderr. Written with no interior whitespace so the wsh tokenizer
# passes it through as a single argument.
LAUNCH_CFG='{"console":{"in":{"name":"null"},"out":{"name":"platform"},"err":{"name":"platform"}}}'

OUT=$(printf 'write /dev/wanted/wapps/%s/config %s\nstart %s\nstatus\nexit\n' \
        "$WAPP_NAME" "$LAUNCH_CFG" "$WAPP_NAME" \
      | timeout 15 "$WANTED" "$CONFIG" 2>&1)
RC=$?

fail() { echo "FAIL: $1"; echo "--- output ---"; printf '%s\n' "$OUT"; exit 1; }

# A crash (segfault during the supervisor-respawn churn) shows up as rc >= 128
# or an explicit core-dump line. Acceptable: 0 (clean drain) / 124 (timeout).
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
    fail "unexpected exit code $RC (likely a crash)"
fi
if printf '%s\n' "$OUT" | grep -qiE 'segmentation|core dumped|Fatal:'; then
    fail "crash/fatal marker in engine output"
fi

# Concurrency assertion: the status snapshot lists both wapps, and the launched
# one is live (running, or still starting if the worker thread is mid-spinup).
printf '%s\n' "$OUT" | grep -qE "supervisor[[:space:]]+running" \
    || fail "supervisor not reported running"
printf '%s\n' "$OUT" | grep -qE "$WAPP_NAME[[:space:]]+(running|starting)" \
    || fail "$WAPP_NAME not reported running alongside the supervisor"

echo "PASS: wsh launched '$WAPP_NAME'; engine ran it concurrently with the supervisor (rc=$RC)"
exit 0
