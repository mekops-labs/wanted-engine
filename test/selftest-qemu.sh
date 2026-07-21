#!/bin/bash
# Run the in-WASM selftest suite against a cross-built engine under qemu
# user-mode emulation — the non-x86 lane, without router hardware.
#
# The engine is cross-built from an OpenWRT SDK (the same toolchain the .ipk
# uses), then driven by test/selftest.sh through a wrapper that execs it under
# qemu with the SDK's target rootfs as the loader root. The suite itself is
# arch-independent: wapps and the supervisor are WASM, loaded by path at
# runtime, so only the engine binary changes.
#
# Emulation is faithful enough to reproduce arch-specific engine faults that x86
# does not exhibit — which is the point of this lane.
#
# Usage: test/selftest-qemu.sh <sdk-url-or-dir> [config]
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SDK_ARG="${1:?usage: selftest-qemu.sh <sdk-url-or-dir> [config]}"
CONFIG="${2:-$REPO/test/selftest-config.json}"

# shellcheck source=packaging/openwrt/sdk-env.sh
. "$REPO/packaging/openwrt/sdk-env.sh"

QEMU="qemu-$QEMU_ARCH-static"
command -v "$QEMU" >/dev/null 2>&1 || {
    echo "selftest-qemu: $QEMU not found — the build image installs" >&2
    echo "  qemu-user-static; see docker/Dockerfile." >&2
    exit 1
}
[ -d "$SDK_TARGET_ROOT" ] || {
    echo "selftest-qemu: no target rootfs under $OPENWRT_SYSROOT" >&2
    exit 1
}

# TLS off: the lane exercises engine/wapp lifecycle, and skipping it avoids the
# SDK's one-time OpenSSL stage. Secure sockets are a configuration symbol, so
# turn them off there — the old -DSECURE_SOCKETS=OFF no longer reaches the
# build, which would have pulled OpenSSL back in.
log "cross-building selftest engine for $OPKG_ARCH"
bdir="$REPO/build-selftest-$OPKG_ARCH"
# WAMR detects stack overflow with guard pages and a SIGSEGV handler. qemu-user
# emulates that imperfectly and the outcome depends on the host kernel's mmap
# behaviour, so the runtime can take a real SIGSEGV before the supervisor emits
# anything. Software bound checking costs a little speed and keeps the lane
# testing the engine rather than the emulator.
mkdir -p "$bdir"
PYTHONPATH="$REPO/tools/kconfiglib" KCONFIG_CONFIG="$bdir/.config" \
    python3 "$REPO/tools/kconfiglib/olddefconfig.py" "$REPO/Kconfig" >/dev/null
PYTHONPATH="$REPO/tools/kconfiglib" KCONFIG_CONFIG="$bdir/.config" \
    python3 "$REPO/tools/kconfiglib/setconfig.py" --kconfig "$REPO/Kconfig" \
    WANTED_VFS_SOCKET_TLS=n >/dev/null

cmake -B "$bdir" -S "$REPO" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$REPO/cmake/toolchain-openwrt.cmake" \
      -DBUILD_TESTING=OFF \
      -DWAMR_DISABLE_HW_BOUND_CHECK=1
cmake --build "$bdir" -j"$(nproc)"

# selftest.sh execs the engine directly, so hand it a wrapper rather than
# teaching it about emulation.
runner="$bdir/wanted-cli-qemu"
cat > "$runner" <<EOF
#!/bin/sh
exec $QEMU -L "$SDK_TARGET_ROOT" "$bdir/cmd/wanted-cli" "\$@"
EOF
chmod +x "$runner"

log "selftest under $QEMU ($OPKG_ARCH)"
exec "$REPO/test/selftest.sh" "$runner" "$CONFIG"
