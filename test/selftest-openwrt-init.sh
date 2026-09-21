#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Packaging + init-script check for the OpenWRT `.ipk` — the automated
# coverage the M5 record's manual GL.iNet run left as a gap ("the QEMU leg
# still installs no package, so the init script remains untested in CI").
#
# Two things are checked, both real (no reimplementation of either):
#   1. The .ipk's data.tar.gz lays out exactly the files packaging promises.
#   2. /etc/init.d/wanted's own render_config() — sourced unmodified, not
#      reimplemented — merges UCI into the packaged base config correctly,
#      with and without an operator-pinned manager/registry.
#
# What this does NOT check: actually starting the service via procd. procd
# needs a real init system (ubus, a running PID 1) that a qemu-user rootfs
# skeleton does not provide — this lane runs one binary under emulation, not
# a full OpenWRT boot. See packaging/openwrt/README.md.
#
# Runs inside the engine toolchain image (docker/Dockerfile installs
# qemu-user-static), writing to real /sbin, /usr/bin, /usr/share/libubox,
# /lib and /etc paths — the init script and uci.sh's own compatibility layer
# hardcode those, so this must run in a throwaway container, not a real host.
#
# Usage: selftest-openwrt-init.sh <wanted-cli> <qemu-binary-name> <uci-root> <opkg-arch>
set -euo pipefail

USAGE="usage: selftest-openwrt-init.sh <wanted-cli> <qemu> <uci-root> <opkg-arch>"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
WANTED_CLI="${1:?$USAGE}"
QEMU="${2:?$USAGE}"
UCI_ROOT="${3:?$USAGE}"
OPKG_ARCH="${4:?$USAGE}"

die() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

[ -w /etc ] || die "run inside a throwaway container — this writes /sbin, /etc, /usr/share/libubox for real"
[ -x "$UCI_ROOT/sbin/uci" ] || die "no cross-built uci at $UCI_ROOT/sbin/uci (STAGE_UCI=1 in sdk-env.sh)"
[ -x "$UCI_ROOT/usr/bin/jshn" ] || die "no cross-built jshn at $UCI_ROOT/usr/bin/jshn"
command -v "$QEMU" >/dev/null 2>&1 || die "$QEMU not found"

# ---- wire the real uci/jshn binaries + real shell libs into place --------

mkdir -p /sbin /usr/bin /usr/share/libubox /lib/config /etc/wanted /etc/config
cat > /sbin/uci <<EOF
#!/bin/sh
exec $QEMU -L "$UCI_ROOT" "$UCI_ROOT/sbin/uci" "\$@"
EOF
chmod +x /sbin/uci
cat > /usr/bin/jshn <<EOF
#!/bin/sh
exec $QEMU -L "$UCI_ROOT" "$UCI_ROOT/usr/bin/jshn" "\$@"
EOF
chmod +x /usr/bin/jshn
cp "$UCI_ROOT/usr/share/libubox/jshn.sh" /usr/share/libubox/jshn.sh
cp "$UCI_ROOT/lib/config/uci.sh" /lib/config/uci.sh
# functions.sh (config_load/config_get/config/option) belongs to base-files,
# not uci — plain shell, no build needed, taken straight from the feed source
# STAGE_UCI already pulled in.
FEED_BASE_FILES="$(ls -d "$REPO"/.openwrt-sdk/*/feeds/base/package/base-files 2>/dev/null | head -1)"
[ -f "$FEED_BASE_FILES/files/lib/functions.sh" ] || die "base-files feed not found under .openwrt-sdk — run sdk-env.sh with STAGE_UCI=1 first"
cp "$FEED_BASE_FILES/files/lib/functions.sh" /lib/functions.sh

# ---- 1. package the real .ipk and check its layout ------------------------

log() { printf '\n=== %s ===\n' "$*"; }
log "packaging: build .ipk and check data.tar.gz's layout"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# No wapp runs in this check — a placeholder tar is enough to exercise
# packaging's own layout, which is all this step verifies.
sup="$work/supervisor.tar"
( d="$work/sup" && mkdir -p "$d" && : > "$d/app.wasm" && tar -C "$d" -cf "$sup" app.wasm )

mkdir -p "$work/out"
"$REPO/packaging/openwrt/make-ipk.sh" "$OPKG_ARCH" "$WANTED_CLI" "$sup" "selftest" \
    "$work/out" "$REPO/configs/openwrt.json" >/dev/null
built_ipk="$(ls "$work/out"/*.ipk | head -1)"
[ -f "$built_ipk" ] || die "make-ipk.sh produced no .ipk"

data_dir="$work/data"
mkdir -p "$data_dir"
tar -xOf "$built_ipk" ./data.tar.gz | tar -xzf - -C "$data_dir"

for f in usr/bin/wanted-cli usr/share/wanted/supervisor.tar etc/wanted/config.json \
         etc/init.d/wanted etc/config/wanted; do
    [ -e "$data_dir/$f" ] || die ".ipk carries no $f"
done
[ -x "$data_dir/etc/init.d/wanted" ] || die "etc/init.d/wanted is not executable in the package"
echo "PASS: the .ipk installs wanted-cli, the supervisor image, and the init/UCI files where packaging promises"

# ---- 2. render_config(), sourced unmodified from the real init script -----

log "init script: render_config() merges UCI into the packaged base config"

cp "$data_dir/etc/wanted/config.json" /etc/wanted/config.json
INIT_SCRIPT="$data_dir/etc/init.d/wanted"

render() { # $1 = UCI body (may be empty) -> writes /var/run/wanted/config.json
    printf 'config wanted main\n%s' "$1" > /etc/config/wanted
    bash -c '
        logger() { :; }
        . /lib/functions.sh
        . /lib/config/uci.sh
        . /usr/share/libubox/jshn.sh
        BASE_CONFIG=/etc/wanted/config.json
        RUN_CONFIG=/var/run/wanted/config.json
        DATADIR=/srv/wanted
        rm -f "$RUN_CONFIG"
        . "'"$INIT_SCRIPT"'"
        render_config
        cat "$RUN_CONFIG"
    '
}

out="$(render '')"
echo "$out" | grep -q '"sockets"' && die "an unconfigured UCI (no manager/registry) rendered a sockets key — should be absent so the provisioning blob's overlay applies"
echo "$out" | grep -q '"imagePath": *"/srv/wanted/supervisor.tar"' || die "the packaged base config's own fields did not survive the render"
echo "PASS: an unconfigured device renders no sockets key — takes both addresses from the provisioning blob"

out="$(render '	option manager tcp://10.0.0.5:9443
	option registry tcp://10.0.0.5:9080
')"
echo "$out" | grep -q '"name": "manager"' || die "a UCI-set manager address did not reach the rendered config"
echo "$out" | grep -q '"tcp://10.0.0.5:9443"' || die "the UCI manager address itself did not reach the rendered config"
echo "$out" | grep -q '"name": "registry"' || die "a UCI-set registry address did not reach the rendered config"
echo "PASS: an operator-pinned manager/registry in UCI renders into the launch config's sockets[]"
