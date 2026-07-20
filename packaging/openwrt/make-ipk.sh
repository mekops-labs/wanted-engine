#!/bin/sh
# Assemble an OpenWRT .ipk from a cross-built engine. Installed layout and the
# supervisor model: packaging/openwrt/README.md.
# Usage: make-ipk.sh <opkg_arch> <wanted-cli> <supervisor.tar> <version> <out_dir>
set -eu

ARCH="$1"; BIN="$2"; SUP="$3"; VER="$4"; OUT="$5"
HERE="$(cd "$(dirname "$0")" && pwd)"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# --- data.tar.gz: the installed rootfs -----------------------------------
data="$work/data"
mkdir -p "$data/usr/bin" "$data/usr/share/wanted" "$data/etc/wanted" "$data/etc/init.d"
install -m 0755 "$BIN" "$data/usr/bin/wanted-cli"
install -m 0644 "$SUP" "$data/usr/share/wanted/supervisor.tar"
install -m 0644 "$HERE/files/etc/wanted/config.json" "$data/etc/wanted/config.json"
install -m 0755 "$HERE/files/etc/init.d/wanted" "$data/etc/init.d/wanted"

isize=$(du -sb "$data" | cut -f1)
( cd "$data" && tar --numeric-owner --owner=0 --group=0 -czf "$work/data.tar.gz" ./* )

# --- control.tar.gz: metadata + maintainer scripts -----------------------
ctrl="$work/control"
mkdir -p "$ctrl"

# Depends follow what the binary actually links.
needed=$(readelf -d "$BIN" 2>/dev/null | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p')
deps="libc"
printf '%s\n' "$needed" | grep -q '^libgcc_s'        && deps="$deps, libgcc"
printf '%s\n' "$needed" | grep -q '^libatomic'       && deps="$deps, libatomic"
printf '%s\n' "$needed" | grep -qE '^lib(ssl|crypto)' && deps="$deps, libopenssl"

cat > "$ctrl/control" <<EOF
Package: wanted-engine
Version: $VER
Depends: $deps
Source: wanted-engine
Section: utils
Architecture: $ARCH
Installed-Size: $isize
Description: WANTED engine — a WebAssembly nanocontainer VFS router.
 Runs sandboxed WebAssembly applications (wapps) under a built-in supervisor;
 provisioned from /etc/wanted/config.json and managed by procd.
EOF

# Preserve user edits to the config across upgrades.
printf '/etc/wanted/config.json\n' > "$ctrl/conffiles"

cat > "$ctrl/postinst" <<'EOF'
#!/bin/sh
[ -n "$IPKG_INSTROOT" ] && exit 0
/etc/init.d/wanted enable
/etc/init.d/wanted restart
exit 0
EOF
cat > "$ctrl/prerm" <<'EOF'
#!/bin/sh
[ -n "$IPKG_INSTROOT" ] && exit 0
/etc/init.d/wanted stop
/etc/init.d/wanted disable
exit 0
EOF
chmod 0755 "$ctrl/postinst" "$ctrl/prerm"
( cd "$ctrl" && tar --numeric-owner --owner=0 --group=0 -czf "$work/control.tar.gz" ./* )

# --- assemble the ipk -----------------------------------------------------
# An OpenWRT .ipk is a gzipped tar, not a Debian ar archive.
printf '2.0\n' > "$work/debian-binary"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
pkg="$OUT/wanted-engine_${VER}_${ARCH}.ipk"
rm -f "$pkg"
( cd "$work" && tar --format=gnu --numeric-owner --owner=0 --group=0 --sort=name \
    --mtime=@0 -cf - ./debian-binary ./data.tar.gz ./control.tar.gz | gzip -n > "$pkg" )

echo "built $pkg ($(du -h "$pkg" | cut -f1))"
