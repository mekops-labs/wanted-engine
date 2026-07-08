#!/bin/bash
# RP2350 M6: validate the signed-firmware pipeline entirely offline. Signs the
# built NuttX/WANTED image with picotool (secp256k1 + SHA-256, RP2350's actual
# bootrom scheme), confirms the signature verifies, then confirms a tampered
# copy of the same image reports an invalid signature.
#
# Deliberately does NOT touch OTP or a device: no `picotool otp load`, no
# `--device`. RP2350 OTP fuses are physically one-way (0->1, unrecoverable),
# and the bootrom's own signature enforcement is gated entirely by an
# OTP-derived flag with no dry-run mode (see research/
# rp2350-secure-boot-signature-verification-without-otp.md) - this script
# proves the signing/verification tooling is correct, not that the chip itself
# would refuse an unsigned image, which requires that OTP burn and is out of
# scope here.
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
# Flip a byte inside the first UF2 block's actual payload (bytes 32..287 of a
# 512-byte block; header is bytes 0..31, the rest of the 476-byte payload area
# past the real 256-byte payload is padding a UF2 reader ignores - flipping a
# byte there changes nothing and silently defeats this check).
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
