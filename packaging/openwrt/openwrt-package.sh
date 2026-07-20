#!/bin/bash
# Build a production OpenWRT .ipk: download/extract the SDK, stage libopenssl,
# cross-build with TLS, package. See packaging/openwrt/README.md.
# Usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK_ARG="${1:?usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]}"
SUPERVISOR="${2:-$REPO/wasm/supervisor/wsh/supervisor.tar}"
OUT="$REPO/dist"

STAGE_SSL=1
# shellcheck source=packaging/openwrt/sdk-env.sh
. "$REPO/packaging/openwrt/sdk-env.sh"

# --- cross-build the engine with SSL --------------------------------------
log "cross-building engine (SSL) for $OPKG_ARCH"
bdir="$REPO/build-openwrt-$OPKG_ARCH"
rm -rf "$bdir"
cmake -B "$bdir" -S "$REPO" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-openwrt.cmake" \
      -DBUILD_TESTING=OFF -C "$REPO/cmake/profiles/small.cmake" \
      -DWANTED_SUPERVISOR_IMAGE_PATH=/usr/share/wanted/supervisor.tar
cmake --build "$bdir" -j"$(nproc)"

# --- assemble the .ipk ----------------------------------------------------
ver="$(cd "$REPO" && git describe --tags 2>/dev/null | sed 's/^v//' || echo 0.0.0)"
sh "$REPO/packaging/openwrt/make-ipk.sh" "$OPKG_ARCH" "$bdir/cmd/wanted-cli" \
   "$SUPERVISOR" "$ver" "$OUT"
