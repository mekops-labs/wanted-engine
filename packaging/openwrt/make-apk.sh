#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Assemble an OpenWrt .apk from a cross-built engine with the SDK's own apk.
# Installed layout and the supervisor model: packaging/openwrt/README.md.
# Usage: make-apk.sh <sdk_dir> <pkg_arch> <wanted-cli> <supervisor.tar>
#                    <version> <out_dir> [config.json]
set -eu

SDK="$1"; ARCH="$2"; BIN="$3"; SUP="$4"; VER="$5"; OUT="$6"
HERE="$(cd "$(dirname "$0")" && pwd)"
# The launch config the package installs; the router one by default, so a direct
# invocation still produces a working package.
CFG="${7:-$HERE/../../configs/openwrt.json}"
NAME=wanted-engine
APK="$SDK/staging_dir/host/bin/apk"
HOST="$SDK/staging_dir/host"

[ -x "$APK" ] || { echo "make-apk.sh: no apk in $SDK (an OpenWrt 25.12 SDK?)" >&2; exit 1; }
"$APK" version --check "$VER" >/dev/null 2>&1 ||
    { echo "make-apk.sh: '$VER' is not a valid apk version" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- the installed rootfs --------------------------------------------------
root="$work/root"
mkdir -p "$root/usr/bin" "$root/usr/share/wanted" "$root/etc/wanted" "$root/etc/init.d" \
         "$root/etc/config"
install -m 0755 "$BIN" "$root/usr/bin/wanted-cli"
install -m 0644 "$SUP" "$root/usr/share/wanted/supervisor.tar"
install -m 0644 "$CFG" "$root/etc/wanted/config.json"
install -m 0755 "$HERE/files/etc/init.d/wanted" "$root/etc/init.d/wanted"
install -m 0644 "$HERE/files/etc/config/wanted" "$root/etc/config/wanted"

# --- OpenWrt's per-package metadata, as the SDK's own package rule writes it
meta="$root/lib/apk/packages"
mkdir -p "$meta"
( cd "$root" && find . -type f,l -printf '/%P\n' | sort ) > "$work/list"
mv "$work/list" "$meta/$NAME.list"
# Edits to these survive an upgrade and a sysupgrade.
printf '/etc/wanted/config.json\n/etc/config/wanted\n' > "$meta/$NAME.conffiles"
while read -r f; do
    printf '%s %s\n' "$f" "$(sha256sum "$root$f" | cut -d' ' -f1)"
done < "$meta/$NAME.conffiles" > "$meta/$NAME.conffiles_static"

# --- maintainer scripts ----------------------------------------------------
cat > "$work/post-install" <<'EOF'
#!/bin/sh
[ "${IPKG_NO_SCRIPT}" = "1" ] && exit 0
[ -n "${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/wanted enable
/etc/init.d/wanted restart
exit 0
EOF
cat > "$work/pre-deinstall" <<'EOF'
#!/bin/sh
[ -n "${IPKG_INSTROOT}" ] && exit 0
/etc/init.d/wanted stop
/etc/init.d/wanted disable
exit 0
EOF

# Depends follow what the binary actually links.
needed=$(readelf -d "$BIN" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
# jshn renders the runtime config from UCI in the init script.
deps="libc jshn"
printf '%s\n' "$needed" | grep -q '^libgcc_s'        && deps="$deps libgcc"
printf '%s\n' "$needed" | grep -q '^libatomic'       && deps="$deps libatomic"
printf '%s\n' "$needed" | grep -qE '^lib(ssl|crypto)' && deps="$deps libopenssl3"

# --- assemble the apk ------------------------------------------------------
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
pkg="$OUT/$NAME-$VER.apk"
rm -f "$pkg"
# fakeroot records every file as root-owned when a user, not root, builds it.
"$HOST/bin/fakeroot" -l "$HOST/lib/libfakeroot.so" -f "$HOST/bin/faked" -- \
    "$APK" mkpkg \
    --info "name:$NAME" \
    --info "version:$VER" \
    --info "arch:$ARCH" \
    --info "description:WANTED engine: a WebAssembly nanocontainer VFS router" \
    --info "license:Apache-2.0" \
    --info "origin:$NAME" \
    --info "url:https://gitlab.com/mekops/wanted/wanted-engine" \
    --info "depends:$deps" \
    --script "post-install:$work/post-install" \
    --script "post-upgrade:$work/post-install" \
    --script "pre-deinstall:$work/pre-deinstall" \
    --files "$root" \
    --output "$pkg"

echo "built $pkg ($(du -h "$pkg" | cut -f1))"
