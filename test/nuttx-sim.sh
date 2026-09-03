#!/bin/bash
# Build and test the WANTED engine on the NuttX simulator. nuttx and nuttx-apps
# are our own forks, pinned as shallow git submodules under third_party/.
#
# Usage: test/nuttx-sim.sh {deps|build|kernel|selftest|syscontrol|clean|all}
#
# Env overrides (the defaults keep everything under the engine checkout):
#   ENGINE_DIR  engine checkout (default: this script's parent)
#   NUTTX_DIR   NuttX RTOS submodule   (default: $ENGINE_DIR/third_party/nuttx)
#   APPS_DIR    nuttx-apps submodule (default: $ENGINE_DIR/third_party/
#               nuttx-apps)
#   SIMROOT     sim launch dir / hostfs root for /data (default:
#               $ENGINE_DIR/build-nuttx/simroot)
#   NUTTX_SKIP_BUILD=1  run-only: skip deps + kernel build, just stage + run
#                       against a prebuilt $NUTTX_DIR/nuttx (split-CI run jobs)
#   NUTTX_CLEAN=1       force a full distclean + reconfigure before building
#   NUTTX_DEPS_FORCE=1  reset the forks even when they carry local work, which
#                       deps refuses to discard by default
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

# Link the engine/wamr sources into the nuttx-apps app package. Each fork's
# checked-out commit is left as-is, and only the source symlinks are
# checkout-location specific, so they are created here and left untracked.
# Refuse to run when a fork carries work this function would destroy. Both
# paths below are lossy: the scrub removes the module store, taking any local
# commits with it, and `submodule update --force` discards tracked changes.
# Neither announces itself, so a build can silently link sources the checkout
# no longer has. Returns 1 and explains; NUTTX_DEPS_FORCE=1 skips the check.
check_forks_disposable() {
    local sm status dirty rc=0

    for sm in third_party/nuttx third_party/nuttx-apps; do
        status=$(git -C "$ENGINE_DIR" submodule status -- "$sm" 2>/dev/null || true)
        case "$status" in
            # Absent or unreadable: nothing to lose, and the scrub is the point.
            ''|-*) continue ;;
            # A different commit than the pin — local commits live here.
            +*)
                echo "ERROR: $sm is not at its pinned commit:" >&2
                echo "    ${status#?}" >&2
                echo "  Push the fork and move the pin, or re-run with" >&2
                echo "  NUTTX_DEPS_FORCE=1 to discard it." >&2
                rc=1
                continue
                ;;
        esac

        # At the pin, so only tracked edits are at risk: `update --force`
        # reverts those and leaves untracked build output alone.
        dirty=$(git -C "$ENGINE_DIR/$sm" status --porcelain -uno 2>/dev/null || true)
        if [ -n "$dirty" ]; then
            echo "ERROR: $sm has uncommitted changes:" >&2
            echo "$dirty" | sed 's/^/    /' >&2
            echo "  Commit them and move the pin, or re-run with" >&2
            echo "  NUTTX_DEPS_FORCE=1 to discard them." >&2
            rc=1
        fi
    done

    return $rc
}

deps() {
    if [ "${NUTTX_DEPS_FORCE:-0}" != 1 ]; then
        check_forks_disposable
    fi

    # The forks are excluded from CI's recursive submodule fetch, so a worktree
    # can survive a prior run without its module store and `submodule update
    # --init` would abort cloning into it. Scrub any inconsistent leftover.
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
    # a restored-from-cache tree whose submodule git dir is absent; the links
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

# Compile the configured JSON in as bytes, since NuttX has no filesystem holding
# a config at first boot. It lands in src/include, on both this build's and
# CMake's include path.
default_config_header() {
    local dotconfig cfg
    dotconfig=$ENGINE_DIR/${BUILD_DIR:-build}/.config
    cfg=$(sed -nE 's/^CONFIG_WANTED_DEFAULT_CONFIG="(.*)"$/\1/p' \
        "$dotconfig" 2>/dev/null || true)
    # The sim stages its launch config into hostfs at run time, so this header
    # goes unused there and the split-CI kernel job builds with no .config at
    # all. A board has no such filesystem and boots on the header alone, so
    # falling back there would ship firmware carrying a config nobody chose.
    if [ -z "$cfg" ]; then
        case "${NUTTX_BOARD:-sim:wanted}" in
            sim:*) cfg=configs/example_config.json ;;
            *)
                echo "no CONFIG_WANTED_DEFAULT_CONFIG in $dotconfig —" \
                    "configure that build dir before building a board" >&2
                exit 1
                ;;
        esac
    fi
    "$ENGINE_DIR/utils/default-config-header.sh" "$ENGINE_DIR" \
        "$cfg" "$ENGINE_DIR/src/include/wanted-config.h"
    echo "generated wanted-config.h from $cfg"
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

# Configure and build a NuttX board config, defaulting to the sim:wanted native
# defconfig in the nuttx fork.
#
# In:  NUTTX_BOARD — overrides it, passed to configure.sh verbatim in its own
#                    <board>:<config> notation; this script enumerates no boards
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
    local apps_rel board marker last_board
    apps_rel=$(realpath --relative-to="$NUTTX_DIR" "$APPS_DIR")
    board=${NUTTX_BOARD:-sim:wanted}
    cd "$NUTTX_DIR"
    # A NuttX board's own .config lives in this one shared checkout, not per
    # BUILD_DIR — reconfigure whenever the requested board differs from the
    # one last configured here, the same guard cmake/Kconfig.cmake keeps for
    # WANTED_DEFCONFIG. Without it, switching boards silently keeps building
    # the previous board's kernel.
    marker=.wanted-board-configured
    last_board=""
    [ -f "$marker" ] && last_board=$(cat "$marker")
    if [ "${NUTTX_CLEAN:-0}" = 1 ] || [ ! -f .config ] || [ "$board" != "$last_board" ]; then
        make distclean >/dev/null 2>&1 || true
        ./tools/configure.sh -a "$apps_rel" "$board" >/dev/null
        echo "$board" > "$marker"
    fi
    # DEFCONFIG names an engine envelope, from which the app Makefile generates
    # the engine's configuration header; unset, the Kconfig defaults apply. That
    # header is a prerequisite of every engine object.
    make -j"$(nproc)" WANTED_DEFCONFIG="${DEFCONFIG:+${DEFCONFIG}_defconfig}"
}

# Stage the current variant's hostfs and rebuild the kernel. Only the sim has a
# host filesystem to stage onto; a hardware board bakes the supervisor into a
# boot ROMFS instead.
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

# Package wapps/<name> into the sim's hostfs registry as <name>@<ver>.wapp; the
# engine resolves that registry as ./registry relative to /data. An image is
# just app.wasm plus any TarFS payload, and identity comes from the filename.
#
# Usage: stage_test_wapp <name>:<ver>
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
    # instances (reader/writer); each picks its side from the ROLE env var in
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

    # A cleanly exited supervisor is respawned, so the sim never self-exits.
    # Run it in the background and stop as soon as the TAP plan line appears,
    # with tail -f streaming the console live rather than buffering it.
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
    # Judge only the first complete run: a respawn can leave a second, partial
    # run in the log, killed mid-flight. Truncate at the first plan line so a
    # `not ok` from that half-run is not counted.
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

# Build the sim with the wsh supervisor and exercise poweroff, reboot and exit
# over the console, mirroring test/syscontrol.sh. The sim has no BOARDIOC_RESET,
# so reboot falls through to a poweroff; only exit must respawn wsh.
syscontrol() {
    SUPERVISOR_VARIANT=wsh
    SUPERVISOR_TAR=$ENGINE_DIR/wasm/supervisor/wsh/supervisor.tar
    build

    local rc=0 ok marker="Following commands are available" banner="Wsh v "
    # Everything below keys on deterministic console signals and process
    # liveness, polled up to generous caps rather than fixed sleeps. The engine
    # samples its control flags once a second, so the caps are loose by design.
    local boot_tenths=600        # 60s for the sim to print the wsh banner
    local act_tenths=300         # 30s for an exit / respawn to take effect

    SIM_OUT=""; SIM_PID=0
    # Boot the sim with a console FIFO held open, so stdin never hits a
    # premature EOF, and block until wsh prints its banner. A non-zero return is
    # a boot failure, kept distinct from a feature failure.
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

    # 1. poweroff -> the engine returns from its loop and the sim exits.
    if boot_sim; then
        send poweroff
        if wait_exit; then echo "ok   - poweroff exits the sim (no respawn)";
        else echo "FAIL - poweroff did not exit the sim"; rc=1; fi
    else boot_fail "poweroff"; fi
    stop_sim; rm -f "$SIM_OUT"

    # 2. exit: the supervisor exits and is respawned with a working console.
    #    'help' is sent only after the respawned banner appears, so the listing
    #    can come only from the new wsh.
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

    # 3. reboot -> the sim has no in-process re-exec path (a board reset
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
