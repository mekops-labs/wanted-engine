#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Build a production OpenWRT .apk: download/extract the SDK, stage libopenssl,
# cross-build with TLS, package. Configuration is the build dir's .config, with
# no defconfig applied implicitly. See packaging/openwrt/README.md.
#
# Usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK_ARG="${1:?usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]}"
OUT="$REPO/dist/openwrt"

# Where the .apk installs it; the engine must be compiled to read it there.
PACKAGED_IMAGE="/usr/share/wanted/supervisor.tar"

DOTCONFIG="${WANTED_CONFIG:-${BUILD_DIR:-build}/.config}"
case "$DOTCONFIG" in /*) ;; *) DOTCONFIG="$REPO/$DOTCONFIG" ;; esac
if [ ! -f "$DOTCONFIG" ]; then
    echo "openwrt-package: no configuration at $DOTCONFIG" >&2
    echo "  configure the build first: just menuconfig" >&2
    echo "  (or seed the router envelope: just defconfig openwrt)" >&2
    exit 1
fi

# A configuration choice; an explicit image argument overrides it.
supervisor_variant() {
    local v
    for v in sheriff wsh selftest; do
        if grep -q "^CONFIG_WANTED_SUPERVISOR_$(printf '%s' "$v" | tr 'a-z' 'A-Z')=y" \
                "$DOTCONFIG"; then
            printf '%s' "$v"
            return 0
        fi
    done
    return 1
}

# sdk-env.sh defines this too, but it is sourced after the checks below.
log() { printf '\n=== %s ===\n' "$*"; }

# --- resolve the supervisor image -----------------------------------------
if [ $# -ge 2 ]; then
    SUPERVISOR="$2"
    log "supervisor image: $SUPERVISOR (explicit)"
else
    if ! variant="$(supervisor_variant)"; then
        echo "openwrt-package: no supervisor variant selected in $DOTCONFIG" >&2
        exit 1
    fi
    SUPERVISOR="$REPO/wasm/supervisor/$variant/supervisor.tar"
    log "supervisor image: $variant (configured)"
fi
if [ ! -f "$SUPERVISOR" ]; then
    echo "openwrt-package: supervisor image not built: $SUPERVISOR" >&2
    echo "  build it with: make ${variant:-supervisor}" >&2
    exit 1
fi

# --- resolve the SDK ------------------------------------------------------
# After the checks above: this downloads and stages, which is minutes on the
# wrong side of a configuration error. Exports SDK and PKG_ARCH.
STAGE_SSL=1
# shellcheck source=packaging/openwrt/sdk-env.sh
. "$REPO/packaging/openwrt/sdk-env.sh"

# --- cross-build the engine with SSL --------------------------------------
bdir="$REPO/build-openwrt-$PKG_ARCH"
# The SDK is baked into CMakeCache; a different one leaves it stale.
stamp="$bdir/.sdk"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$SDK" ]; then
    log "SDK changed — discarding $bdir"
    rm -rf "$bdir"
fi
mkdir -p "$bdir"
printf '%s\n' "$SDK" > "$stamp"

# `.config.src` records what was copied, so an unchanged source is incremental.
if ! cmp -s "$DOTCONFIG" "$bdir/.config.src"; then
    log "configuration: $DOTCONFIG"
    cp "$DOTCONFIG" "$bdir/.config"
    cp "$DOTCONFIG" "$bdir/.config.src"
fi
# A property of the package, not a user choice: the binary must agree with it.
if ! grep -q "^CONFIG_WANTED_SUPERVISOR_IMAGE_PATH=\"$PACKAGED_IMAGE\"\$" "$bdir/.config"; then
    log "pinning supervisor image path to the packaged $PACKAGED_IMAGE"
    # setconfig takes the value raw; quoting stores the quotes in the path.
    ( cd "$REPO" && PYTHONPATH="$REPO/tools/kconfiglib" KCONFIG_CONFIG="$bdir/.config" \
        python3 "$REPO/tools/kconfiglib/setconfig.py" --kconfig Kconfig \
        "WANTED_SUPERVISOR_IMAGE_PATH=$PACKAGED_IMAGE" )
fi

log "configuring engine build for $PKG_ARCH ($(basename "$bdir"))"
cmake -B "$bdir" -S "$REPO" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-openwrt.cmake" \
      -DBUILD_TESTING=OFF

log "cross-building engine (SSL) for $PKG_ARCH"
cmake --build "$bdir" -j"$(nproc)"

# --- assemble the .apk ----------------------------------------------------
log "packaging .apk"
# Becomes the package's /etc/wanted/config.json; validated before packaging.
cfg_sym=$(sed -nE 's/^CONFIG_WANTED_DEFAULT_CONFIG="(.*)"$/\1/p' "$DOTCONFIG")
CONFIG_JSON="$("$REPO/utils/default-config.sh" "$REPO" "${cfg_sym:-configs/example_config.json}")"
log "default configuration: ${cfg_sym:-configs/example_config.json}"
# A tagged, clean checkout packages as the tag alone (e.g. "0.16.0"). Anything
# else — a commit past the tag, or uncommitted changes — packages as an apk
# development version: a build timestamp and the short hash after the tag,
# "0.16.0_git20260908143022~1234567". apk sorts it after the tag and before the
# next release; it has no place for a dirty marker, so the timestamp keeps two
# builds from an unclean tree apart.
build_stamp="$(date -u +%Y%m%d%H%M%S)"
raw="$(cd "$REPO" && git describe --tags --dirty --long --match 'v*' 2>/dev/null || true)"
if [[ "$raw" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)-([0-9]+)-g([0-9a-f]+)(-dirty)?$ ]]; then
    tag="${BASH_REMATCH[1]}"
    count="${BASH_REMATCH[2]}"
    hash="${BASH_REMATCH[3]}"
    dirty="${BASH_REMATCH[4]}"
    if [ "$count" = "0" ] && [ -z "$dirty" ]; then
        ver="$tag"
    else
        ver="${tag}_git${build_stamp}~${hash}"
    fi
else
    # No reachable "v*" tag at all: always a development version, never a bare
    # "0.0.0" that could collide with a real early release.
    hash="$(cd "$REPO" && git rev-parse --short=7 HEAD 2>/dev/null || echo 0000000)"
    ver="0.0.0_git${build_stamp}~${hash}"
fi
sh "$REPO/packaging/openwrt/make-apk.sh" "$SDK" "$PKG_ARCH" "$bdir/cmd/wanted-cli" \
   "$SUPERVISOR" "$ver" "$OUT" "$CONFIG_JSON"
