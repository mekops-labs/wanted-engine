#!/bin/bash
# Build and test the WANTED engine on the NuttX simulator.
#
#
# Usage: test/nuttx-sim.sh {deps|build|selftest|syscontrol|clean|all}
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

# Link the engine/wamr sources into the nuttx-apps app package. The sim board
# config (boards/sim/.../configs/wanted) lives in the nuttx fork and the app
# package (system/wanted: Make.defs/Makefile/Kconfig) in the nuttx-apps fork.
# The forks' checked-out commit is left as-is — whatever the user has in
# third_party/nuttx{,-apps} is used; this does not move or re-pin the submodules.
# Only the engine/wamr source symlinks are checkout-location specific, so create
# those in the app package here and keep them out of the apps submodule status.
deps() {
    # The NuttX + apps forks are excluded from CI's default recursive submodule
    # fetch (only this job needs them, and they are large). Shallow-init them
    # here when absent; a no-op on a tree that already has them checked out, so
    # a local checkout's pinned commit is left as-is.
    if [ ! -f "$NUTTX_DIR/Makefile" ] || [ ! -f "$APPS_DIR/Makefile" ]; then
        git -C "$ENGINE_DIR" submodule update --init --depth 1 \
            third_party/nuttx third_party/nuttx-apps
    fi

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

# Stage the supervisor image + engine config into the sim's hostfs root (/data).
# Variant-specific (selftest vs wsh) and cheap, so every phase stages its own.
stage_hostfs() {
    [ -f "$SUPERVISOR_TAR" ] || \
        { echo "missing $SUPERVISOR_TAR (run 'make supervisor')" >&2; exit 1; }
    mkdir -p "$SIMROOT/wanted"
    cp "$SUPERVISOR_TAR" "$SIMROOT/wanted/supervisor.tar"
    cp "$ENGINE_DIR/test/nuttx-sim-config.json" "$SIMROOT/smoke.json"
}

# Configure + build the sim:wanted board config (the native defconfig in the
# nuttx fork: SYSTEM_WANTED + hostfs, wanted_sim_main as the init task, and
# BOARDCTL_POWEROFF so the sim exits cleanly when the engine loop returns).
#
# The supervisor variant is staged into hostfs and loaded at runtime, so the
# kernel binary is identical across phases: configure once (on a fresh or
# `clean`ed tree) and let make rebuild incrementally on later calls. This builds
# the kernel a single time per `all` run and makes repeated local phase runs
# near-instant. Set NUTTX_CLEAN=1 to force a full reconfigure.
build_kernel() {
    local apps_rel
    apps_rel=$(realpath --relative-to="$NUTTX_DIR" "$APPS_DIR")
    cd "$NUTTX_DIR"
    if [ "${NUTTX_CLEAN:-0}" = 1 ] || [ ! -f .config ]; then
        make distclean >/dev/null 2>&1 || true
        ./tools/configure.sh -a "$apps_rel" sim:wanted >/dev/null
    fi
    make -j"$(nproc)"
}

# Stage the current variant's hostfs and (re)build the kernel.
build() {
    stage_hostfs
    build_kernel
}

# Package a test wapp into the sim's hostfs registry, which the engine resolves
# as ./registry relative to /data (wanted_sim_main chdirs there).
#
# stage_test_wapp <name>:<ver>
# Package wapps/<name> as <name>:<ver>.wapp. An image is just app.wasm (+ any
# TarFS payload); identity comes from the registry filename. The `duplex` image
# is launched as two instances (reader/writer) by the supervisor via config
# `image`, so it is staged once, not aliased. Mirrors the Linux `stage` helper
# in test/selftest.sh.
stage_test_wapp() {
    local name=${1%%:*} ver=${1#*:} s
    make -C "$ENGINE_DIR/wapps/$name" >/dev/null 2>&1
    mkdir -p "$SIMROOT/registry"
    s=$(mktemp -d)
    cp "$ENGINE_DIR/wapps/$name/$name.wasm" "$s/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$SIMROOT/registry/$name:$ver.wapp" app.wasm
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
    stage_test_wapp argenv:0.0.1-1
    # The inter-wapp pipe round-trip runs the single `duplex` image as two
    # instances (reader/writer); each picks its side from the ROLE env var in its
    # launch config. The supervisor binds the image via config `image`.
    stage_test_wapp duplex:0.0.1-1
    stage_test_wapp volcheck:0.0.1-1
    # hand-crafted malformed images for the loader-robustness check (reuse the
    # valid wasm that stage_test_wapp just built)
    "$ENGINE_DIR/test/stage-malformed.sh" "$SIMROOT/registry" \
        "$ENGINE_DIR/wapps/trapper/trapper.wasm"

    # The volume persistence check asserts a fresh store on its first run; the
    # engine roots volumes at ./data under SIMROOT, so clear any leftover from a
    # prior local run (CI starts from a clean checkout).
    rm -rf "$SIMROOT/data"

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
    # Judge only the first complete run: the engine respawns a cleanly-exited
    # supervisor, so the log can hold a second, partial run that was killed
    # mid-flight once the first plan line appeared. Truncate at the first plan
    # line so a half-run check (`not ok` from a killed respawn) is not counted.
    out=$(sed '/^1\.\./q' "$log"); rm -f "$log"
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

# Build the sim with the wsh supervisor and exercise the system-control paths
# (poweroff / reboot / exit) over the console, mirroring test/syscontrol.sh on
# Linux. The sim has no BOARDIOC_RESET, so reboot falls through to a poweroff —
# both end the run without respawning; only the exit case must respawn wsh, and
# it must come back with a working console (the borrowed-stdio fix).
syscontrol() {
    SUPERVISOR_VARIANT=wsh
    SUPERVISOR_TAR=$ENGINE_DIR/wasm/supervisor/wsh/supervisor.tar
    build

    local rc=0 marker="Following commands are available"

    # Boot the sim with a console FIFO, run the timed "delay:cmd" steps, and set
    # SIM_ALIVE (1/0) + SIM_OUT. The sim is killed by PID, never orphaned.
    SIM_OUT=""; SIM_ALIVE=0
    drive_sim() {
        local fifo ep
        SIM_OUT=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
        ( cd "$SIMROOT" && exec "$NUTTX_DIR/nuttx" ) <"$fifo" >"$SIM_OUT" 2>&1 &
        ep=$!
        exec 9>"$fifo"
        local spec
        for spec in "$@"; do sleep "${spec%%:*}"; printf '%s\n' "${spec#*:}" >&9; done
        sleep 2
        if kill -0 "$ep" 2>/dev/null; then SIM_ALIVE=1; else SIM_ALIVE=0; fi
        exec 9>&-
        kill -9 "$ep" 2>/dev/null || true
        wait "$ep" 2>/dev/null || true
        rm -f "$fifo"
    }

    drive_sim "2:poweroff"
    if [ "$SIM_ALIVE" -eq 0 ]; then echo "ok   - poweroff exits the sim (no respawn)";
    else echo "FAIL - poweroff did not exit the sim"; rc=1; fi
    rm -f "$SIM_OUT"

    drive_sim "2:exit" "3:help"
    if [ "$SIM_ALIVE" -eq 1 ] && grep -q "$marker" "$SIM_OUT"; then
        echo "ok   - exit respawns wsh with a working console"
    else echo "FAIL - exit did not respawn with a working console"; rc=1; fi
    rm -f "$SIM_OUT"

    drive_sim "2:reboot"
    if [ "$SIM_ALIVE" -eq 0 ]; then echo "ok   - reboot does not respawn (sim resets/exits)";
    else echo "FAIL - reboot left the sim respawning"; rc=1; fi
    rm -f "$SIM_OUT"

    if [ "$rc" -eq 0 ]; then echo "PASS: syscontrol on the NuttX sim";
    else echo "FAIL: syscontrol on the NuttX sim"; exit 1; fi
}

for cmd in "${@:-all}"; do
    case "$cmd" in
        deps)       deps ;;
        build)      build ;;
        selftest)   deps; selftest ;;
        syscontrol) deps; syscontrol ;;
        clean)      make -C "$NUTTX_DIR" distclean >/dev/null 2>&1 || true ;;
        all)        deps; selftest; syscontrol ;;
        *) echo "usage: $0 [deps|build|selftest|syscontrol|clean|all ...]" >&2; exit 2 ;;
    esac
done
