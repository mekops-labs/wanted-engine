#!/bin/bash
# Inter-wapp named-pipe smoke test: prove /dev/pipe/<name> is a process-wide
# IPC channel by exchanging a payload between two app-wapps through one pipe.
#
# Flow:
#   1. Build the `hello` sample wapp once, then package its wasm into TWO
#      registry images under REGISTRY_ROOT (./wapps) — `hello-reader` and
#      `hello-writer`. Same binary; the role is injected at launch.
#   2. Boot wanted-cli with the wsh supervisor and feed it:
#        - a launch config for each instance whose config-map driver mounts the
#          role string ("reader"/"writer") at /dev/role; the reader also gets a
#          host preopen at RESULT_DIR to drop its result file;
#        - `start hello-reader` — the reader blocks reading /dev/pipe/smoke;
#        - `start hello-writer` — the writer writes PAYLOAD to the same pipe;
#        - `status`, then `exit`.
#   3. The reader unblocks, receives PAYLOAD across the wapp boundary, and
#      writes it to RESULT_FILE. Assert that file equals PAYLOAD after the CLI
#      drains. The result file (not stdout) is the observation point: the stdio
#      teardown drops a launched wapp's late stdout.
#
# This fails on the old per-wapp-isolated pipe driver (the reader and writer
# get independent ring buffers, so the reader blocks to its safety cap and the
# result file stays empty) and passes on the shared store.
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$PROJECT_ROOT"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./docs/example_config_smoke.json}

WAPP_SRC_DIR=wapps/hello
WAPP_VERSION=0.0.1-1
REGISTRY_ROOT=./wapps
READER_NAME=hello-reader
WRITER_NAME=hello-writer
READER_IMAGE=$REGISTRY_ROOT/$READER_NAME:$WAPP_VERSION.wapp
WRITER_IMAGE=$REGISTRY_ROOT/$WRITER_NAME:$WAPP_VERSION.wapp

# Must match wapps/hello/hello.c.
PAYLOAD="inter-wapp-pipe-ok"
RESULT_DIR=/tmp/wanted-smoke-pipe
RESULT_FILE=$RESULT_DIR/result

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli binary not found at $WANTED"
    exit 1
fi
if [ ! -f "$CONFIG" ]; then
    echo "FAIL: smoke config not found at $CONFIG"
    exit 1
fi

# Build the wapp wasm once (idempotent).
if ! make -C "$WAPP_SRC_DIR" >/dev/null 2>&1; then
    echo "FAIL: could not build $WAPP_SRC_DIR (wasi-sdk toolchain required)"
    exit 1
fi

STAGE=$(mktemp -d)
cleanup() {
    rm -rf "$STAGE"
    rm -f "$READER_IMAGE" "$WRITER_IMAGE"
    rm -rf "$RESULT_DIR"
}
trap cleanup EXIT

# Package one registry image per role. The manifest name must equal the image
# basename so the registry resolves `start <name>` to it; the same app.wasm
# backs both.
package_image() {
    name=$1
    image=$2
    cp "$WAPP_SRC_DIR/hello.wasm" "$STAGE/app.wasm"
    printf '{"name":"%s","version":[0,0,1],"package":1,"requirements":["console"]}\n' \
        "$name" > "$STAGE/manifest.json"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$STAGE" -cf "$image" app.wasm manifest.json
}
package_image "$READER_NAME" "$READER_IMAGE"
package_image "$WRITER_NAME" "$WRITER_IMAGE"

rm -rf "$RESULT_DIR"

# Launch configs (no interior whitespace so the wsh tokenizer passes each as a
# single argument). The config-map driver mounts the role at /dev/role.
READER_CFG='{"console":{"in":{"name":"null"},"out":{"name":"platform"},"err":{"name":"platform"}},"drivers":[{"name":"config","path":"/dev/role","options":"reader"}],"preopens":["'$RESULT_DIR'"]}'
WRITER_CFG='{"console":{"in":{"name":"null"},"out":{"name":"platform"},"err":{"name":"platform"}},"drivers":[{"name":"config","path":"/dev/role","options":"writer"}]}'

# Reader first (it blocks on the pipe), then the writer (it produces the data).
OUT=$(printf 'write /dev/wanted/wapps/%s/config %s\nwrite /dev/wanted/wapps/%s/config %s\nstart %s\nstart %s\nstatus\nexit\n' \
        "$READER_NAME" "$READER_CFG" \
        "$WRITER_NAME" "$WRITER_CFG" \
        "$READER_NAME" "$WRITER_NAME" \
      | timeout 15 "$WANTED" "$CONFIG" 2>&1)
RC=$?

fail() { echo "FAIL: $1"; echo "--- output ---"; printf '%s\n' "$OUT"; exit 1; }

# Acceptable: 0 (clean drain) / 124 (timeout during supervisor respawn churn).
if [ "$RC" -ne 0 ] && [ "$RC" -ne 124 ]; then
    fail "unexpected exit code $RC (likely a crash)"
fi
if printf '%s\n' "$OUT" | grep -qiE 'segmentation|core dumped|Fatal:'; then
    fail "crash/fatal marker in engine output"
fi

# The real assertion: the reader received the writer's payload over the pipe.
if [ ! -f "$RESULT_FILE" ]; then
    fail "result file $RESULT_FILE was not written (reader never received data)"
fi
GOT=$(cat "$RESULT_FILE")
if [ "$GOT" != "$PAYLOAD" ]; then
    fail "result mismatch: expected '$PAYLOAD', got '$GOT'"
fi

echo "PASS: '$WRITER_NAME' → /dev/pipe/smoke → '$READER_NAME' delivered '$GOT' (rc=$RC)"
exit 0
