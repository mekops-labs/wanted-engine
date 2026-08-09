#!/bin/bash
# Validates the signed-firmware pipeline entirely offline: sign the built image
# with picotool, confirm it verifies, then confirm a tampered copy does not.
# It touches no OTP and no device, since RP2350 fuses are physically one-way.
#
# Usage: test/rp2350-sign-verify.sh [path-to-nuttx.uf2]
#        (runs in the build container; see `make rp2350-sign`)
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"

FW=${1:-third_party/nuttx/nuttx.uf2}
KEYDIR=keys/rp2350-dev
KEY="$KEYDIR/signing_key.pem"

[ -f "$FW" ] || { echo "FAIL: missing $FW (run 'make rp2350' first)"; exit 1; }

mkdir -p "$KEYDIR"
if [ ! -f "$KEY" ]; then
    echo "-- no dev signing key at $KEY, generating one --"
    openssl ecparam -name secp256k1 -genkey -out "$KEY" || {
        echo "FAIL: key generation"
        exit 1
    }
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

SIGNED="$WORK/signed.uf2"
TAMPERED="$WORK/tampered.uf2"

rc=0

echo "=== sign $FW ==="
if ! picotool seal --sign --major 1 --minor 0 "$FW" "$SIGNED" "$KEY" 2>&1; then
    echo "FAIL: picotool seal --sign"
    exit 1
fi

echo "=== verify the signed image reports valid ==="
out=$(picotool info -a "$SIGNED" 2>&1)
echo "$out" | sed 's/^/  /'
if echo "$out" | grep -q '^ *signature: *verified$'; then
    echo "PASS: correctly-signed image verifies"
else
    echo "FAIL: correctly-signed image did not report 'verified'"
    rc=1
fi

echo "=== tamper a copy and confirm verification fails ==="
cp "$SIGNED" "$TAMPERED"
# Flip a byte inside the first UF2 block's actual payload, bytes 32..287 of the
# 512-byte block. The rest of the payload area is padding a reader ignores, so
# flipping a byte there would silently defeat this check.
python3 -c "
with open('$TAMPERED', 'r+b') as f:
    f.seek(40)
    b = f.read(1)
    f.seek(40)
    f.write(bytes([b[0] ^ 0xFF]))
"
out=$(picotool info -a "$TAMPERED" 2>&1)
echo "$out" | sed 's/^/  /'
if echo "$out" | grep -q '^ *signature: *incorrect$'; then
    echo "PASS: tampered image is correctly rejected"
else
    echo "FAIL: tampered image did not report 'incorrect' (verification is not actually checking the payload)"
    rc=1
fi

[ "$rc" -eq 0 ] && echo "PASS: rp2350-sign-verify (signed-firmware pipeline validated offline, no OTP touched)"
exit "$rc"
