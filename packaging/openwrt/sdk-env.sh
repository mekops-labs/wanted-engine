#!/bin/bash
# Resolve an OpenWRT SDK and export the cross-build environment for it.
# Sourced (not executed) by openwrt-package.sh and test/selftest-qemu.sh.
#
# In:  SDK_ARG   — an architecture keyword (aarch64 | mipsel), an SDK URL, or
#                  an already-extracted SDK directory
#      REPO      — repository root
#      STAGE_SSL — "1" to stage libopenssl into the SDK (once per SDK)
# Out: SDK, SDK_TARGET_ROOT, OPKG_ARCH, QEMU_ARCH, plus the OPENWRT_* and
#      STAGING_DIR variables cmake/toolchain-openwrt.cmake reads.

SDK_CACHE="$REPO/.openwrt-sdk"

log() { printf '\n=== %s ===\n' "$*"; }

# Generic per-architecture SDKs. Deliberately board-independent targets: armsr
# is "ARM SystemReady" and malta is the QEMU reference board, so a lane built
# from these proves the architecture rather than one router. Same release and
# toolchain for both, so a difference between them is a difference in the arch.
OPENWRT_RELEASE="24.10.0"
openwrt_sdk_url() { # $1 = target path, $2 = target slug
    printf 'https://downloads.openwrt.org/releases/%s/targets/%s/openwrt-sdk-%s-%s_gcc-13.3.0_musl.Linux-x86_64.tar.zst' \
        "$OPENWRT_RELEASE" "$1" "$OPENWRT_RELEASE" "$2"
}
case "$SDK_ARG" in
    aarch64) SDK_ARG="$(openwrt_sdk_url armsr/armv8 armsr-armv8)" ;;
    mipsel)  SDK_ARG="$(openwrt_sdk_url malta/le malta-le)" ;;
esac

# --- resolve the SDK (download+extract a URL, or use a directory) ---------
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
            *) echo "unknown SDK archive type: $fname" >&2; return 1 ;;
        esac
        rm -f "$SDK_CACHE/$fname"
    else
        log "SDK already extracted: $dname"
    fi
else
    echo "SDK arg is neither a URL nor a directory: $SDK_ARG" >&2; return 1
fi

# --- auto-detect the target from the SDK layout ---------------------------
tc_dir="$(basename "$(ls -d "$SDK"/staging_dir/toolchain-* | head -1)")"
tgt_dir="$(basename "$(ls -d "$SDK"/staging_dir/target-* | head -1)")"
cross="$(basename "$(ls "$SDK/staging_dir/$tc_dir/bin/"*-openwrt-linux-*-gcc | head -1)")"
cross="${cross%-gcc}"
OPKG_ARCH="${tgt_dir#target-}"; OPKG_ARCH="${OPKG_ARCH%_musl}"; OPKG_ARCH="${OPKG_ARCH%_glibc}"
case "$cross" in
    aarch64-*) wamr_arch=aarch64; QEMU_ARCH=aarch64 ;;
    mipsel-*)  wamr_arch=mips;    QEMU_ARCH=mipsel ;;
    mips-*)    wamr_arch=mips;    QEMU_ARCH=mips ;;
    *) echo "unsupported arch '$cross' — extend cmake/toolchain-openwrt.cmake and this case" >&2; return 1 ;;
esac
log "target: cross=$cross opkg_arch=$OPKG_ARCH wamr_arch=$wamr_arch"

# Loader root for the cross-built binary — where qemu's -L resolves the dynamic
# linker and shared libraries.
#
# A board SDK stages a populated target rootfs (staging_dir/target-*/root-<x>).
# A generic one (armsr, malta) has no board to stage for and ships none, but its
# toolchain sysroot carries the musl loader and libc, which is all the engine
# needs. Prefer the rootfs when present, fall back to the toolchain.
# `|| true`: with no match the pipeline fails, and the caller runs under
# `set -eo pipefail`, so the assignment alone would abort before the fallback.
SDK_TARGET_ROOT="$(ls -d "$SDK/staging_dir/$tgt_dir"/root-* 2>/dev/null | head -1 || true)"
if [ -z "$SDK_TARGET_ROOT" ]; then
    SDK_TARGET_ROOT="$SDK/staging_dir/$tc_dir"
    log "no staged rootfs; using the toolchain sysroot as the loader root"
fi

# --- stage libopenssl into the SDK (once) ---------------------------------
if [ "${STAGE_SSL:-0}" = "1" ]; then
    if ls "$SDK"/staging_dir/target-*/usr/lib/libssl.so.* >/dev/null 2>&1; then
        log "OpenSSL already staged in SDK"
    else
        log "staging OpenSSL into SDK (one-time per SDK)"
        if ! awk 'BEGIN { a[1] = 1; asort(a) }' >/dev/null 2>&1; then
            echo "sdk-env: gawk (asort) missing — build image lacks the" >&2
            echo "  OpenWRT SDK prerequisites; see docker/Dockerfile." >&2
            return 1
        fi
        # OpenWRT refuses to build as root; drop privileges when we have them.
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
fi

export STAGING_DIR="$SDK/staging_dir"
export OPENWRT_TOOLCHAIN="$SDK/staging_dir/$tc_dir"
export OPENWRT_CROSS="$cross"
export OPENWRT_SYSROOT="$SDK/staging_dir/$tgt_dir"
export OPENWRT_ARCH="$wamr_arch"
