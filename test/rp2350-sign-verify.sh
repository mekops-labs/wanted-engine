#!/bin/bash
# Validates the signed-firmware pipeline entirely offline: sign the built image
# with picotool, confirm it verifies, then confirm a tampered copy does not.
# It touches no OTP and no device, since RP2350 fuses are physically one-way.
#
# The second half covers the artifact a staged update actually ships: the same
# image marked try-before-you-buy. The mark is patched into the IMAGE_DEF
# before sealing, so the signature covers it -- and a signed image that loses
# its mark, or a marked image that loses its signature, would each boot wrongly
# on a secure board, so both are asserted on one artifact.
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

echo "=== sign an image already marked try-before-you-buy ==="
TBYB_BIN="$WORK/tbyb.bin"
TBYB_SIGNED="$WORK/tbyb-signed.bin"
TBYB_TAMPERED="$WORK/tbyb-tampered.bin"

if ! python3 utils/picobin-tbyb.py "$FW" "$TBYB_BIN" 2>&1; then
    echo "FAIL: could not set the try-before-you-buy bit"
    exit 1
fi
if ! picotool seal --hash --sign --major 1 --minor 0 \
        "$TBYB_BIN" -t bin -o 0x10000000 "$TBYB_SIGNED" -t bin "$KEY" 2>&1; then
    echo "FAIL: picotool seal --sign on a try-before-you-buy image"
    exit 1
fi

out=$(picotool info -a "$TBYB_SIGNED" -t bin 2>&1)
echo "$out" | sed 's/^/  /'
if echo "$out" | grep -q '^ *signature: *verified$' &&
   echo "$out" | grep -q '^ *tbyb: *not bought$'; then
    echo "PASS: one artifact is both signed and marked try-before-you-buy"
else
    echo "FAIL: signed try-before-you-buy image did not report both"
    rc=1
fi

echo "=== tamper the signed try-before-you-buy image ==="
cp "$TBYB_SIGNED" "$TBYB_TAMPERED"
# A raw bin, so byte 40 is payload directly rather than inside a UF2 block.
python3 -c "
with open('$TBYB_TAMPERED', 'r+b') as f:
    f.seek(40)
    b = f.read(1)
    f.seek(40)
    f.write(bytes([b[0] ^ 0xFF]))
"
out=$(picotool info -a "$TBYB_TAMPERED" -t bin 2>&1)
echo "$out" | sed 's/^/  /'
if echo "$out" | grep -q '^ *signature: *incorrect$'; then
    echo "PASS: tampered try-before-you-buy image is correctly rejected"
else
    echo "FAIL: tampered try-before-you-buy image did not report 'incorrect'"
    rc=1
fi

[ "$rc" -eq 0 ] && echo "PASS: rp2350-sign-verify (signed-firmware pipeline validated offline, no OTP touched)"
exit "$rc"
