#!/bin/bash
# Supervisor live-update functional test: replace the supervisor image without
# stopping the engine or its child wapps. Asserts child continuity, armed-only
# adoption, and rollback to the built-in image.
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

# A wapp image that loads and runs, then exits after a couple of seconds. It
# stands in for a supervisor that starts under an engine it cannot work with:
# the load succeeds, so only the lifetime distinguishes it.
stage_fast_exit() {
    local d
    d=$(mktemp -d)
    cp wapps/hello/hello.wasm "$d/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$d" -cf "$STAGED.new" app.wasm
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

# 3. a staged image is adopted only when a reload is armed. Control for the
# rollback case: stage a bad image but arm no reload, and the engine must keep
# using the image it already read.
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

# 5. a staged image that loads but exits at once rolls back. This image is valid
# wasm and reaches its entry point, so the load never fails and no launch error
# is reported; its lifetime is the only thing that gives it away.
stage "$WSH_TAR"
engine_start
send 1   "help"
stage_fast_exit
send 1   "write /dev/wanted/ctl reload-supervisor"
send 0.5 "exit"
send 25  "help"
engine_stop

[ "$ENGINE_ALIVE" -eq 1 ]
check $? "engine stays up when the staged supervisor exits at once"

grep -q "falling back to the built-in image" "$ENGINE_OUT"
check $? "a staged supervisor that exits at once rolls back"

[ "$(grep -c 'Following commands are available' "$ENGINE_OUT")" -ge 2 ]
check $? "console works again after a fast-exit rollback"
rm -f "$ENGINE_OUT"

# 6. the supervisor image comes from the wapp registry. A control plane can
# install into the registry and nowhere else, thus this is the source a board
# with no writable image path uses.
sup_reg="$REGISTRY_ROOT/sup@0.0.1-1.wapp"
s=$(mktemp -d)
tar -xf "$WSH_TAR" -C "$s"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$s" -cf "$sup_reg" app.wasm
rm -rf "$s"

REG_CONFIG="$STAGE_DIR/registry-supervisor.json"
sed 's|"imagePath": "[^"]*"|"imagePath": "registry:sup"|' "$CONFIG" > "$REG_CONFIG"

ENGINE_OUT=$(mktemp)
FIFO=$(mktemp -u)
mkfifo "$FIFO"
"$WANTED" "$REG_CONFIG" <"$FIFO" >"$ENGINE_OUT" 2>&1 &
ENGINE_PID=$!
exec 9>"$FIFO"
send 2 "help"
send 1 "exit"
sleep 2
kill "$ENGINE_PID" 2>/dev/null
wait "$ENGINE_PID" 2>/dev/null
exec 9>&-
rm -f "$FIFO"

grep -q "Following commands are available" "$ENGINE_OUT"
check $? "the supervisor runs from a registry image"

! grep -q "failed to load supervisor image" "$ENGINE_OUT"
check $? "a registry-sourced supervisor needs no fallback to the built-in image"
rm -f "$ENGINE_OUT"

# 7. a reload adopts what the registry holds now. The replacement exits at
# once, thus the rollback is what proves the new bytes were read.
ENGINE_OUT=$(mktemp)
FIFO=$(mktemp -u)
mkfifo "$FIFO"
"$WANTED" "$REG_CONFIG" <"$FIFO" >"$ENGINE_OUT" 2>&1 &
ENGINE_PID=$!
exec 9>"$FIFO"
send 2 "help"
s=$(mktemp -d)
cp wapps/hello/hello.wasm "$s/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$s" -cf "$sup_reg.new" app.wasm
mv -f "$sup_reg.new" "$sup_reg"
rm -rf "$s"
send 1  "write /dev/wanted/ctl reload-supervisor"
send 0.5 "exit"
send 25 "help"
sleep 2
kill "$ENGINE_PID" 2>/dev/null
wait "$ENGINE_PID" 2>/dev/null
exec 9>&-
rm -f "$FIFO"

grep -q "falling back to the built-in image" "$ENGINE_OUT"
check $? "a reload adopts the image the registry holds now"
rm -f "$ENGINE_OUT" "$sup_reg"

if [ "$rc" -eq 0 ]; then
    echo "PASS: live-update (child continuity / image adoption / rollback / registry source)"
else
    echo "FAIL: live-update"
fi
exit $rc
