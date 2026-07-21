#!/bin/bash
# Build a production OpenWRT .ipk: download/extract the SDK, stage libopenssl,
# cross-build with TLS, package. See packaging/openwrt/README.md.
# Usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]
#
# Configuration is the build dir's .config, whatever `just menuconfig` last
# wrote; no defconfig is applied implicitly. WANTED_CONFIG/BUILD_DIR selects it.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK_ARG="${1:?usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]}"
OUT="$REPO/dist"

# Where the .ipk installs it; the engine must be compiled to read it there.
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
# wrong side of a configuration error. Exports SDK and OPKG_ARCH.
STAGE_SSL=1
# shellcheck source=packaging/openwrt/sdk-env.sh
. "$REPO/packaging/openwrt/sdk-env.sh"

# --- cross-build the engine with SSL --------------------------------------
bdir="$REPO/build-openwrt-$OPKG_ARCH"
# The SDK is baked into CMakeCache; a different one leaves it stale.
stamp="$bdir/.sdk"
if [ -f "$stamp" ] && [ "$(cat "$stamp")" != "$SDK" ]; then
    log "SDK changed — discarding $bdir"
    rm -rf "$bdir"
fi
mkdir -p "$bdir"
printf '%s\n' "$SDK" > "$stamp"

# `.config.src` records what was copied, so an unchanged source stays incremental.
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

log "configuring engine build for $OPKG_ARCH ($(basename "$bdir"))"
cmake -B "$bdir" -S "$REPO" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-openwrt.cmake" \
      -DBUILD_TESTING=OFF

log "cross-building engine (SSL) for $OPKG_ARCH"
cmake --build "$bdir" -j"$(nproc)"

# --- assemble the .ipk ----------------------------------------------------
log "packaging .ipk"
ver="$(cd "$REPO" && git describe --tags 2>/dev/null | sed 's/^v//' || echo 0.0.0)"
sh "$REPO/packaging/openwrt/make-ipk.sh" "$OPKG_ARCH" "$bdir/cmd/wanted-cli" \
   "$SUPERVISOR" "$ver" "$OUT"
