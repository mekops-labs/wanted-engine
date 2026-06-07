#!/bin/bash
# Build and test the WANTED engine on the NuttX simulator.
#
#
# Usage: test/nuttx-sim.sh {deps|build|selftest|all}
#
# nuttx + nuttx-apps are the mekops forks (wanted + devices changes), pinned as
# shallow git submodules under third_party/. Env overrides (defaults keep
# everything under the engine — no host layout assumptions):
#   ENGINE_DIR  engine checkout (default: this script's parent)
#   NUTTX_DIR   NuttX RTOS submodule   (default: $ENGINE_DIR/third_party/nuttx)
#   APPS_DIR    nuttx-apps submodule   (default: $ENGINE_DIR/third_party/nuttx-apps)
#   SIMROOT     sim launch dir / hostfs root for /data (default: $ENGINE_DIR/build-nuttx/simroot)
set -euo pipefail

ENGINE_DIR=${ENGINE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
NUTTX_DIR=${NUTTX_DIR:-$ENGINE_DIR/third_party/nuttx}
APPS_DIR=${APPS_DIR:-$ENGINE_DIR/third_party/nuttx-apps}
SIMROOT=${SIMROOT:-$ENGINE_DIR/build-nuttx/simroot}
# Boot supervisor variant staged as the sim's supervisor image (wsh for the
# interactive shell / plain build; selftest for the in-WASM test suite).
SUPERVISOR_VARIANT=${SUPERVISOR_VARIANT:-wsh}
SUPERVISOR_TAR=$ENGINE_DIR/wasm/supervisor/$SUPERVISOR_VARIANT/supervisor.tar

# Shallow-init the pinned NuttX + apps forks (wanted branch). The sim board
# config (boards/sim/.../configs/wanted) lives in the nuttx fork and the app
# package (system/wanted: Make.defs/Makefile/Kconfig) in the nuttx-apps fork.
# Only the engine/wamr source symlinks are checkout-location specific, so create
# those in the app package here and keep them out of the apps submodule status.
deps() {
    git -C "$ENGINE_DIR" submodule update --init --depth 1 -- \
        third_party/nuttx third_party/nuttx-apps

    local appdir="$APPS_DIR/system/wanted"
    ( cd "$appdir"
      rel=$(realpath --relative-to="$appdir" "$ENGINE_DIR")
      ln -sfn "$rel"             engine
      ln -sfn "$rel/vendor/wamr" wamr )

    local gd
    gd=$(git -C "$APPS_DIR" rev-parse --absolute-git-dir)
    for ex in '/system/wanted/engine' '/system/wanted/wamr'; do
        grep -qxF "$ex" "$gd/info/exclude" 2>/dev/null || \
            echo "$ex" >> "$gd/info/exclude"
    done
    echo "linked engine sources into $appdir"
}

# Configure + build the sim:wanted board config (the native defconfig in the
# nuttx fork: SYSTEM_WANTED + hostfs, wanted_sim_main as the init task, and
# BOARDCTL_POWEROFF so the sim exits cleanly when the engine loop returns), and
# stage the supervisor image + config into the hostfs root (/data).
build() {
    [ -f "$SUPERVISOR_TAR" ] || \
        { echo "missing $SUPERVISOR_TAR (run 'make supervisor')" >&2; exit 1; }
    mkdir -p "$SIMROOT/wanted"
    cp "$SUPERVISOR_TAR" "$SIMROOT/wanted/supervisor.tar"
    cp "$ENGINE_DIR/test/nuttx-sim-config.json" "$SIMROOT/smoke.json"

    local apps_rel
    apps_rel=$(realpath --relative-to="$NUTTX_DIR" "$APPS_DIR")
    cd "$NUTTX_DIR"
    make distclean >/dev/null 2>&1 || true
    ./tools/configure.sh -a "$apps_rel" sim:wanted >/dev/null
    make -j"$(nproc)"
}

# Package a test wapp (wapps/<name>) into the sim's hostfs registry, which the
# engine resolves as ./wapps relative to /data (wanted_sim_main chdirs there).
stage_test_wapp() {
    local name=${1%%:*} ver=${1#*:} s
    make -C "$ENGINE_DIR/wapps/$name" >/dev/null 2>&1
    mkdir -p "$SIMROOT/wapps"
    s=$(mktemp -d)
    cp "$ENGINE_DIR/wapps/$name/$name.wasm" "$s/app.wasm"
    cp "$ENGINE_DIR/wapps/$name/manifest.json" "$s/manifest.json"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$SIMROOT/wapps/$name:$ver.wapp" app.wasm manifest.json
    rm -rf "$s"
}

# Build the sim with the selftest supervisor + the launched test wapps, boot
# it, and check the TAP it prints to the console.
selftest() {
    SUPERVISOR_VARIANT=selftest
    SUPERVISOR_TAR=$ENGINE_DIR/wasm/supervisor/selftest/supervisor.tar
    build
    stage_test_wapp trapper:0.0.1-1
    stage_test_wapp looper:0.0.1-1
    stage_test_wapp stackbomb:0.0.1-1
    stage_test_wapp membomb:0.0.1-1
    stage_test_wapp cpuhog:0.0.1-1
    stage_test_wapp blocker:0.0.1-1
    stage_test_wapp pblock:0.0.1-1
    stage_test_wapp escaper:0.0.1-1
    stage_test_wapp fdhog:0.0.1-1
    stage_test_wapp crasher:0.0.1-1
    stage_test_wapp preader:0.0.1-1
    stage_test_wapp pwriter:0.0.1-1
    # hand-crafted malformed images for the loader-robustness check (reuse the
    # valid wasm that stage_test_wapp just built)
    "$ENGINE_DIR/test/stage-malformed.sh" "$SIMROOT/wapps" \
        "$ENGINE_DIR/wapps/trapper/trapper.wasm"

    # The engine keeps running after the test supervisor finishes (a cleanly
    # exited supervisor is respawned for resilience), so the sim won't self-exit.
    # Run it in the background, stop as soon as the TAP plan line appears, and
    # kill it — no waiting out a timeout. tail -f streams the console live so the
    # TAP and phase lines appear as they happen instead of all at once at the end.
    local out log pid tpid
    log=$(mktemp)
    ( cd "$SIMROOT" && exec "$NUTTX_DIR/nuttx" ) >"$log" 2>&1 &
    pid=$!
    tail -n +1 -f "$log" 2>/dev/null &
    tpid=$!
    for _ in $(seq 1 90); do
        grep -qE '^1\.\.' "$log" 2>/dev/null && break
        kill -0 "$pid" 2>/dev/null || break
        sleep 1
    done
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    sleep 1                              # let tail flush the final lines
    kill "$tpid" 2>/dev/null || true
    wait "$tpid" 2>/dev/null || true
    out=$(cat "$log"); rm -f "$log"
    if ! printf '%s\n' "$out" | grep -qE '^1\.\.[0-9]+'; then
        echo "FAIL: no TAP plan (selftest supervisor did not finish on the sim)"
        exit 1
    fi
    if printf '%s\n' "$out" | grep -q '^not ok'; then
        echo "FAIL: a selftest check failed on the sim"
        exit 1
    fi
    echo "PASS: selftest on the NuttX sim"
}

for cmd in "${@:-all}"; do
    case "$cmd" in
        deps)     deps ;;
        build)    build ;;
        selftest) deps; selftest ;;
        all)      deps; selftest ;;
        *) echo "usage: $0 [deps|build|selftest|all ...]" >&2; exit 2 ;;
    esac
done
