#!/bin/bash
# Supervisor live-update functional test: replace the supervisor image without
# stopping the engine or its child wapps. Asserts child continuity, that a
# staged image is adopted only when a reload is armed, and that a bad staged
# image rolls back to the built-in one.
#
# Usage: test/live-update.sh [wanted-cli] [config]
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

WANTED=${1:-./build-wsh/cmd/wanted-cli}
CONFIG=${2:-./test/live-update-config.json}
STAGE_DIR=./build-live-update
STAGED="$STAGE_DIR/staged-supervisor.tar"
WSH_TAR=./wasm/supervisor/wsh/supervisor.tar
REGISTRY_ROOT=${REGISTRY_ROOT:-./registry}

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wsh engine not found at $WANTED (run 'BUILD_DIR=build-wsh just wsh')"
    exit 1
fi
if [ ! -f "$WSH_TAR" ]; then
    echo "FAIL: wsh supervisor image not found at $WSH_TAR (run 'make -C wasm/supervisor')"
    exit 1
fi

rc=0
mkdir -p "$STAGE_DIR" "$REGISTRY_ROOT"

# A long-running child to observe across the update.
staged_wapp="$REGISTRY_ROOT/looper@0.0.1-1.wapp"
s=$(mktemp -d)
cp wapps/looper/looper.wasm "$s/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$s" -cf "$staged_wapp" app.wasm
rm -rf "$s"

cleanup() { rm -rf "$STAGE_DIR" "$staged_wapp"; }
trap cleanup EXIT

# Stage by atomic rename — the engine holds the current image mapped, so an
# in-place overwrite changes what the running engine sees.
stage() {
    cp "$1" "$STAGED.new"
    mv -f "$STAGED.new" "$STAGED"
}

# A structurally valid tar that is not a wapp image: it has no app.wasm, so the
# load fails and the engine must roll back rather than abort.
stage_broken() {
    local d
    d=$(mktemp -d)
    printf 'not-a-wapp' > "$d/README"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$d" -cf "$STAGED.new" README
    rm -rf "$d"
    mv -f "$STAGED.new" "$STAGED"
}

ENGINE_OUT=""
ENGINE_ALIVE=0
FIFO=""
ENGINE_PID=""

engine_start() {
    ENGINE_OUT=$(mktemp)
    FIFO=$(mktemp -u)
    mkfifo "$FIFO"
    "$WANTED" "$CONFIG" <"$FIFO" >"$ENGINE_OUT" 2>&1 &
    ENGINE_PID=$!
    exec 9>"$FIFO"
}

# send <delay> <command>
send() { sleep "$1"; printf '%s\n' "$2" >&9; }

engine_stop() {
    sleep 2
    if kill -0 "$ENGINE_PID" 2>/dev/null; then ENGINE_ALIVE=1; else ENGINE_ALIVE=0; fi
    exec 9>&-
    kill -9 "$ENGINE_PID" 2>/dev/null
    wait "$ENGINE_PID" 2>/dev/null
    rm -f "$FIFO"
}

check() {
    if [ "$1" -eq 0 ]; then
        echo "ok   - $2"
    else
        echo "FAIL - $2"
        rc=1
    fi
}

# ── 1+2. child continuity and image adoption ────────────────────────────────
# Staged image = a copy of wsh, so the respawned supervisor is a working shell.
stage "$WSH_TAR"
engine_start
send 1   "create looper"
send 0.5 'set_config looper {"image":"looper","console":{"out":{"name":"log"}}}'
send 0.5 "start looper"
send 2   "status looper"
send 1   "write /dev/wanted/ctl reload-supervisor"
send 0.5 "exit"
send 5   "status looper"
send 1   "help"
engine_stop

[ "$ENGINE_ALIVE" -eq 1 ]
check $? "engine survives the supervisor swap"

# Two "state running" reads for looper: one before the update, one from the
# respawned supervisor afterwards.
[ "$(grep -c 'state    running' "$ENGINE_OUT")" -ge 2 ]
check $? "child wapp keeps running across the supervisor swap"

# Liveness only — a working console does not prove the staged bytes were
# adopted (this image is a copy of the one already in use). The armed/unarmed
# pair below is what proves adoption.
grep -q "Following commands are available" "$ENGINE_OUT"
check $? "supervisor is serving again after the swap"
rm -f "$ENGINE_OUT"

# ── 3. a staged image is adopted only when a reload is armed ────────────────
# Control for the rollback case: stage a bad image but do NOT arm a reload.
# The engine must keep using the good image it already read, never touch the
# staged path — the reload is what causes the re-read.
stage "$WSH_TAR"
engine_start
send 1   "help"
stage_broken
send 1   "exit"
send 5   "help"
engine_stop

[ "$ENGINE_ALIVE" -eq 1 ]
check $? "respawn without a reload keeps the image already in use"

[ "$(grep -c 'Following commands are available' "$ENGINE_OUT")" -ge 2 ]
check $? "unarmed respawn does not adopt the staged image"

grep -q "falling back to the built-in image" "$ENGINE_OUT" && rollback_seen=1 || rollback_seen=0
[ "$rollback_seen" -eq 0 ]
check $? "unarmed respawn does not roll back (nothing was reloaded)"
rm -f "$ENGINE_OUT"

# ── 4. rollback ─────────────────────────────────────────────────────────────
# Same unloadable image, this time with a reload armed. The respawn reads it,
# fails repeatedly, falls back to the built-in, and the engine keeps serving.
stage "$WSH_TAR"
engine_start
send 1   "help"
stage_broken
send 1   "write /dev/wanted/ctl reload-supervisor"
send 0.5 "exit"
send 12  "help"
engine_stop

[ "$ENGINE_ALIVE" -eq 1 ]
check $? "engine stays up when the staged supervisor cannot launch"

grep -q "falling back to the built-in image" "$ENGINE_OUT"
check $? "engine reports the rollback"

# The banner is printed once per supervisor start; the fallback supervisor must
# have come up and answered a command after the rollback.
[ "$(grep -c 'Following commands are available' "$ENGINE_OUT")" -ge 2 ]
check $? "rolled-back supervisor has a working console"
rm -f "$ENGINE_OUT"

if [ "$rc" -eq 0 ]; then
    echo "PASS: live-update (child continuity / image adoption / rollback)"
else
    echo "FAIL: live-update"
fi
exit $rc
