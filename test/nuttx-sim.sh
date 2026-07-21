#!/bin/bash
# Build and test the WANTED engine on the NuttX simulator.
#
#
# Usage: test/nuttx-sim.sh {deps|build|kernel|selftest|syscontrol|clean|all}
#
# nuttx + nuttx-apps are the mekops forks (wanted + devices changes), pinned as
# shallow git submodules under third_party/. Env overrides (defaults keep
# everything under the engine — no host layout assumptions):
#   ENGINE_DIR  engine checkout (default: this script's parent)
#   NUTTX_DIR   NuttX RTOS submodule   (default: $ENGINE_DIR/third_party/nuttx)
#   APPS_DIR    nuttx-apps submodule   (default: $ENGINE_DIR/third_party/nuttx-apps)
#   SIMROOT     sim launch dir / hostfs root for /data (default: $ENGINE_DIR/build-nuttx/simroot)
#   NUTTX_SKIP_BUILD=1  run-only: skip deps + kernel build, just stage + run
#                       against a prebuilt $NUTTX_DIR/nuttx (split-CI run jobs)
#   NUTTX_CLEAN=1       force a full distclean + reconfigure before building
set -euo pipefail

ENGINE_DIR=${ENGINE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
# shellcheck source=test/lib-wapp.sh
. "$ENGINE_DIR/test/lib-wapp.sh"
WAPP_ROOT=$ENGINE_DIR
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
    # here; a no-op on a tree that is already at the correct pinned commit.
    #
    # CI reuses the project workspace across jobs/pipelines, and because these
    # paths are skipped by the runner's submodule fetch (GIT_SUBMODULE_PATHS in
    # .gitlab-ci.yml), a worktree dir can survive from a prior run without its
    # module store. `git submodule update --init` then tries to *clone* into a
    # non-empty dir and aborts ("destination path ... already exists and is not
    # an empty directory"). Make init idempotent: keep a pristine checkout (the
    # warm cache, status prefix ' '), but scrub any inconsistent leftover —
    # worktree and/or module store — so the clone below starts from clean state.
    local gitdir sm
    gitdir=$(git -C "$ENGINE_DIR" rev-parse --absolute-git-dir)
    for sm in third_party/nuttx third_party/nuttx-apps; do
        case "$(git -C "$ENGINE_DIR" submodule status -- "$sm" 2>/dev/null)" in
            " "*) continue ;;   # present at the pinned commit — leave it (cache hit)
        esac
        git -C "$ENGINE_DIR" submodule deinit -f -- "$sm" >/dev/null 2>&1 || true
        rm -rf "${ENGINE_DIR:?}/$sm" "$gitdir/modules/$sm"
    done
    git -C "$ENGINE_DIR" submodule update --init --recursive --depth 1 --force \
        third_party/nuttx third_party/nuttx-apps

    local appdir="$APPS_DIR/system/wanted"
    ( cd "$appdir"
      rel=$(realpath --relative-to="$appdir" "$ENGINE_DIR")
      ln -sfn "$rel"             engine
      ln -sfn "$rel/vendor/wamr" wamr )

    # Keep the engine/wamr symlinks out of the apps submodule's status. Tolerate
    # a restored-from-cache tree whose submodule git dir is absent — the symlinks
    # above are what the build needs; this exclude is only cosmetic.
    local gd
    gd=$(git -C "$APPS_DIR" rev-parse --absolute-git-dir 2>/dev/null) || gd=""
    if [ -n "$gd" ]; then
        for ex in '/system/wanted/engine' '/system/wanted/wamr'; do
            grep -qxF "$ex" "$gd/info/exclude" 2>/dev/null || \
                echo "$ex" >> "$gd/info/exclude"
        done
    fi
    echo "linked engine sources into $appdir"
    default_config_header
}

# Compile the configured JSON in as bytes — NuttX has no filesystem holding a
# config at first boot. Lands in src/include, which is on both this build's and
# CMake's include path. Here rather than in the fork's app Makefile; every NuttX
# entry point runs deps.
default_config_header() {
    local cfg
    cfg=$(sed -nE 's/^CONFIG_WANTED_DEFAULT_CONFIG="(.*)"$/\1/p' \
        "$ENGINE_DIR/${BUILD_DIR:-build}/.config" 2>/dev/null || true)
    "$ENGINE_DIR/utils/default-config-header.sh" "$ENGINE_DIR" \
        "${cfg:-configs/example_config.json}" \
        "$ENGINE_DIR/src/include/wanted-config.h"
    echo "generated wanted-config.h from ${cfg:-configs/example_config.json}"
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

# Configure + build a NuttX board config. Defaults to sim:wanted (the native
# defconfig in the nuttx fork: SYSTEM_WANTED + hostfs, wanted_sim_main as the
# init task, and BOARDCTL_POWEROFF so the sim exits cleanly when the engine loop
# returns).
#
# NUTTX_BOARD overrides it and is passed to configure.sh verbatim, in its own
# <board>:<config> notation -- this script enumerates no boards. `just build`
# sets it from the engine's configured target; hardware boards additionally need
# a toolchain image the host build image does not carry.
#
build_kernel() {
    # Run-only mode (split CI: a `build` job built the kernel and passed the
    # binary as an artifact; the selftest/syscontrol jobs only stage + run).
    # Assert the prebuilt kernel is present and skip configure/make entirely.
    if [ "${NUTTX_SKIP_BUILD:-0}" = 1 ]; then
        [ -f "$NUTTX_DIR/nuttx" ] || {
            echo "NUTTX_SKIP_BUILD=1 but no prebuilt kernel at $NUTTX_DIR/nuttx" >&2
            exit 1
        }
        return 0
    fi
    local apps_rel
    apps_rel=$(realpath --relative-to="$NUTTX_DIR" "$APPS_DIR")
    cd "$NUTTX_DIR"
    if [ "${NUTTX_CLEAN:-0}" = 1 ] || [ ! -f .config ]; then
        make distclean >/dev/null 2>&1 || true
        ./tools/configure.sh -a "$apps_rel" "${NUTTX_BOARD:-sim:wanted}" >/dev/null
    fi
    # DEFCONFIG names an engine envelope (engine configs/<name>_defconfig); the
    # app Makefile generates the engine's configuration header from it. Unset,
    # the Kconfig defaults apply. Its generated header is a prerequisite of every
    # engine object, so a changed configuration rebuilds what depends on it.
    make -j"$(nproc)" WANTED_DEFCONFIG="${DEFCONFIG:+${DEFCONFIG}_defconfig}"
}

# Stage the current variant's hostfs and (re)build the kernel. Only the sim has
# a host filesystem to stage onto; hardware boards bake the supervisor into a
# boot ROMFS instead, and staging for them would fail on a missing simroot for
# an image nothing reads.
build() {
    case "${NUTTX_BOARD:-sim:wanted}" in
        sim:*) stage_hostfs ;;
    esac
    build_kernel
}

# Build just the kernel (no hostfs staging). Used by the split-CI build-nuttx
# job, which produces the kernel binary as an artifact for the run jobs and has
# no supervisor image to stage.
kernel() {
    deps
    build_kernel
}

# Package a test wapp into the sim's hostfs registry, which the engine resolves
# as ./registry relative to /data (wanted_sim_main chdirs there).
#
# stage_test_wapp <name>:<ver>
# Package wapps/<name> as <name>@<ver>.wapp (the registry version separator is
# '@'; the <name>:<ver> argument is this script's token format). An image is just
# app.wasm (+ any
# TarFS payload); identity comes from the registry filename. The `duplex` image
# is launched as two instances (reader/writer) by the supervisor via config
# `image`, so it is staged once, not aliased. Mirrors the Linux `stage` helper
# in test/selftest.sh.
stage_test_wapp() {
    local name=${1%%:*} ver=${1#*:} s
    wapp_build "$name"
    mkdir -p "$SIMROOT/registry"
    s=$(mktemp -d)
    cp "$ENGINE_DIR/wapps/$name/$name.wasm" "$s/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$SIMROOT/registry/$name@$ver.wapp" app.wasm
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
    stage_test_wapp bigmem:0.0.1-1
    stage_test_wapp biginit:0.0.1-1
    stage_test_wapp observer:0.0.1-1
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

    local rc=0 ok marker="Following commands are available" banner="Wsh v "
    # Everything below is keyed on deterministic console signals (the wsh banner
    # and the help listing) and process liveness, polled up to generous caps —
    # never fixed sleeps. The engine loop only samples its control flags once a
    # second, so poweroff/reboot take a tick or two to return; a loaded CI runner
    # stacks scheduling jitter on top. Tight windows here are what produced the
    # intermittent "did not exit" / "left respawning" false negatives, so the
    # caps are deliberately loose (the happy path returns as soon as the signal
    # appears; only a genuine hang waits the cap out).
    local boot_tenths=600        # 60s for the sim to print the wsh banner
    local act_tenths=300         # 30s for an exit / respawn to take effect

    SIM_OUT=""; SIM_PID=0
    # Boot the sim with a console FIFO held open (no premature stdin EOF) and
    # block until wsh prints its banner. Returns non-zero if the sim never
    # reaches the prompt — an infra/boot failure, kept distinct from a feature
    # failure so a slow/broken boot is never mistaken for a passing control path.
    boot_sim() {
        local fifo _
        SIM_OUT=$(mktemp); fifo=$(mktemp -u); mkfifo "$fifo"
        ( cd "$SIMROOT" && exec "$NUTTX_DIR/nuttx" ) <"$fifo" >"$SIM_OUT" 2>&1 &
        SIM_PID=$!
        exec 9>"$fifo"
        for _ in $(seq 1 "$boot_tenths"); do
            grep -qF "$banner" "$SIM_OUT" 2>/dev/null && return 0
            kill -0 "$SIM_PID" 2>/dev/null || return 1   # sim died during boot
            sleep 0.1
        done
        return 1
    }
    send()      { printf '%s\n' "$1" >&9; }              # one command line to wsh
    sim_alive() { kill -0 "$SIM_PID" 2>/dev/null; }
    stop_sim() {                                         # kill by PID, never orphan
        exec 9>&- 2>/dev/null || true
        kill -9 "$SIM_PID" 2>/dev/null || true
        wait "$SIM_PID" 2>/dev/null || true
    }
    # Poll until the sim process exits (0) or the cap elapses (1).
    wait_exit() {
        local _
        for _ in $(seq 1 "$act_tenths"); do sim_alive || return 0; sleep 0.1; done
        return 1
    }
    # Poll until the banner has appeared at least $1 times (0) or the cap/death
    # ends it (1). A second banner is the deterministic proof of a respawn.
    wait_banner() {
        local want=$1 n _
        for _ in $(seq 1 "$act_tenths"); do
            n=$(grep -cF "$banner" "$SIM_OUT" 2>/dev/null || true)
            [ "${n:-0}" -ge "$want" ] && return 0
            sim_alive || return 1
            sleep 0.1
        done
        return 1
    }
    # Poll until $1 appears in the console (0) or the cap/death ends it (1).
    wait_marker() {
        local _
        for _ in $(seq 1 "$act_tenths"); do
            grep -qF "$1" "$SIM_OUT" 2>/dev/null && return 0
            sim_alive || return 1
            sleep 0.1
        done
        return 1
    }
    boot_fail() {                                        # loud, distinct diagnostic
        echo "FAIL - $1: sim did not boot to a wsh prompt (infra/boot failure)"
        echo "------ last console output ------"; tail -n 25 "$SIM_OUT" 2>/dev/null || true
        echo "--------------------------------"
        rc=1
    }

    # 1. poweroff -> the engine returns from its loop and the sim exits, no respawn.
    if boot_sim; then
        send poweroff
        if wait_exit; then echo "ok   - poweroff exits the sim (no respawn)";
        else echo "FAIL - poweroff did not exit the sim"; rc=1; fi
    else boot_fail "poweroff"; fi
    stop_sim; rm -f "$SIM_OUT"

    # 2. exit -> the supervisor exits and is respawned WITH a working console. The
    #    first wsh only receives 'exit'; 'help' is sent only AFTER the respawned
    #    wsh's banner appears, so the help listing can come only from the new wsh
    #    (and can't be swallowed by the exiting one). Marker + liveness => pass.
    if boot_sim; then
        send exit
        ok=0
        if wait_banner 2; then
            send help
            if sim_alive && wait_marker "$marker"; then ok=1; fi
        fi
        if [ "$ok" -eq 1 ]; then echo "ok   - exit respawns wsh with a working console";
        else echo "FAIL - exit did not respawn with a working console"; rc=1; fi
    else boot_fail "exit"; fi
    stop_sim; rm -f "$SIM_OUT"

    # 3. reboot -> the sim has no in-process re-exec path (a board reset replaces
    #    the whole image), so reboot ends the run like poweroff: no respawn.
    if boot_sim; then
        send reboot
        if wait_exit; then echo "ok   - reboot does not respawn (sim resets/exits)";
        else echo "FAIL - reboot left the sim respawning"; rc=1; fi
    else boot_fail "reboot"; fi
    stop_sim; rm -f "$SIM_OUT"

    if [ "$rc" -eq 0 ]; then echo "PASS: syscontrol on the NuttX sim";
    else echo "FAIL: syscontrol on the NuttX sim"; exit 1; fi
}

for cmd in "${@:-all}"; do
    case "$cmd" in
        deps)       deps ;;
        build)      build ;;
        kernel)     kernel ;;
        selftest)   [ "${NUTTX_SKIP_BUILD:-0}" = 1 ] || deps; selftest ;;
        syscontrol) [ "${NUTTX_SKIP_BUILD:-0}" = 1 ] || deps; syscontrol ;;
        clean)      make -C "$NUTTX_DIR" distclean >/dev/null 2>&1 || true ;;
        all)        deps; selftest; syscontrol ;;
        *) echo "usage: $0 [deps|build|kernel|selftest|syscontrol|clean|all ...]" >&2; exit 2 ;;
    esac
done
