#!/bin/bash
# Build a production OpenWRT .ipk: download/extract the SDK, stage libopenssl,
# cross-build with TLS, package. See packaging/openwrt/README.md.
# Usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SDK_ARG="${1:?usage: openwrt-package.sh <sdk-url-or-dir> [supervisor.tar]}"
SUPERVISOR="${2:-$REPO/wasm/supervisor/wsh/supervisor.tar}"
SDK_CACHE="$REPO/.openwrt-sdk"
OUT="$REPO/dist"

log() { printf '\n=== %s ===\n' "$*"; }

# --- 1. resolve the SDK (download+extract a URL, or use a directory) -------
if [ -d "$SDK_ARG" ]; then
    SDK="$(cd "$SDK_ARG" && pwd)"
elif printf '%s' "$SDK_ARG" | grep -qE '^https?://'; then
    mkdir -p "$SDK_CACHE"
    fname="$(basename "$SDK_ARG")"
    dname="${fname%.tar.*}"
    SDK="$SDK_CACHE/$dname"
    if [ ! -d "$SDK" ]; then
        log "downloading SDK: $fname"
        wget -q -O "$SDK_CACHE/$fname" "$SDK_ARG"
        log "extracting SDK"
        case "$fname" in
            *.tar.zst) tar --zstd -xf "$SDK_CACHE/$fname" -C "$SDK_CACHE" ;;
            *.tar.xz)  tar -xf "$SDK_CACHE/$fname" -C "$SDK_CACHE" ;;
            *) echo "unknown SDK archive type: $fname" >&2; exit 1 ;;
        esac
        rm -f "$SDK_CACHE/$fname"
    else
        log "SDK already extracted: $dname"
    fi
else
    echo "SDK arg is neither a URL nor a directory: $SDK_ARG" >&2; exit 1
fi

# --- 2. auto-detect the target from the SDK layout ------------------------
tc_dir="$(basename "$(ls -d "$SDK"/staging_dir/toolchain-* | head -1)")"
tgt_dir="$(basename "$(ls -d "$SDK"/staging_dir/target-* | head -1)")"
cross="$(basename "$(ls "$SDK/staging_dir/$tc_dir/bin/"*-openwrt-linux-*-gcc | head -1)")"
cross="${cross%-gcc}"
opkg_arch="${tgt_dir#target-}"; opkg_arch="${opkg_arch%_musl}"; opkg_arch="${opkg_arch%_glibc}"
case "$cross" in
    aarch64-*) wamr_arch=aarch64 ;;
    mips*-*)   wamr_arch=mips ;;
    *) echo "unsupported arch '$cross' — extend cmake/toolchain-openwrt.cmake and this case" >&2; exit 1 ;;
esac
log "target: cross=$cross opkg_arch=$opkg_arch wamr_arch=$wamr_arch"

# --- 3. stage libopenssl into the SDK (once) ------------------------------
if ls "$SDK"/staging_dir/target-*/usr/lib/libssl.so.* >/dev/null 2>&1; then
    log "OpenSSL already staged in SDK"
else
    log "staging OpenSSL into SDK (one-time per SDK)"
    # OpenWRT's build system needs host tools the base image lacks: gawk (its
    # scan.awk uses asort) and a distutils shim for the Python prereq. Install
    # them when we have the privilege; otherwise say what's missing. (Proper
    # home for these: the build image itself.)
    if ! awk 'BEGIN { a[1] = 1; asort(a) }' >/dev/null 2>&1; then
        if [ "$(id -u)" -eq 0 ]; then
            apt-get update -qq && apt-get install -y -qq \
                gawk file unzip rsync wget gettext libncurses-dev zlib1g-dev python3-setuptools
            update-alternatives --set awk /usr/bin/gawk
        else
            echo "openwrt-package: missing OpenWRT build prerequisites (gawk with" >&2
            echo "  asort, python3-setuptools, ...) and not running as root." >&2
            echo "  Add them to the build image, or run this step as root." >&2
            exit 1
        fi
    fi
    # OpenWRT refuses to build as root. When we are root, drop to an
    # unprivileged user; under a keep-id user namespace we already are one.
    if [ "$(id -u)" -eq 0 ]; then
        id owrt >/dev/null 2>&1 || useradd -m owrt
        chown -R owrt "$SDK"
        run_sdk() { gosu owrt bash -c "$1"; }
    else
        run_sdk() { bash -c "$1"; }
    fi
    run_sdk "cd '$SDK' && \
        ./scripts/feeds update base >/dev/null 2>&1 && \
        ./scripts/feeds install libopenssl >/dev/null 2>&1 && \
        make defconfig >/dev/null 2>&1 && \
        make package/openssl/compile -j\$(nproc) >/dev/null 2>&1"
fi

# --- 4. cross-build the engine with SSL -----------------------------------
log "cross-building engine (SSL) for $opkg_arch"
export STAGING_DIR="$SDK/staging_dir"
export OPENWRT_TOOLCHAIN="$SDK/staging_dir/$tc_dir"
export OPENWRT_CROSS="$cross"
export OPENWRT_SYSROOT="$SDK/staging_dir/$tgt_dir"
export OPENWRT_ARCH="$wamr_arch"
bdir="$REPO/build-openwrt-$opkg_arch"
rm -rf "$bdir"
cmake -B "$bdir" -S "$REPO" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-openwrt.cmake" \
      -DBUILD_TESTING=OFF -C "$REPO/cmake/profiles/small.cmake" \
      -DWANTED_SUPERVISOR_IMAGE_PATH=/usr/share/wanted/supervisor.tar
cmake --build "$bdir" -j"$(nproc)"

# --- 5. assemble the .ipk -------------------------------------------------
ver="$(cd "$REPO" && git describe --tags 2>/dev/null | sed 's/^v//' || echo 0.0.0)"
sh "$REPO/packaging/openwrt/make-ipk.sh" "$opkg_arch" "$bdir/cmd/wanted-cli" \
   "$SUPERVISOR" "$ver" "$OUT"
