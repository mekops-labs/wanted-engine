#!/bin/bash
# Registry image verification: an image the engine cannot verify must not run
# when enforcement is on, and each refusal must name its own state.
#
# The engine loads a supervisor named by `registry:<ref>`, which puts the load
# check on the path without a control plane. Entries and their metadata records
# are written here directly, so every state is reachable and deterministic.
#
# Usage: image-verify.sh [wanted-cli]
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJECT_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$PROJECT_ROOT" || exit 1

WANTED=${1:-./build/cmd/wanted-cli}
if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli binary not found at $WANTED"
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
REG="$WORK/registry"
mkdir -p "$REG"

PASS=0
FAIL=0
NAME=probe
VER=1.0.0

# write_entry <state> — one registry entry plus the metadata record that puts
# the engine's check into <state>.
write_entry() {
    # Cleared first: a record left by the previous case would decide this one.
    rm -rf "$REG"
    mkdir -p "$REG"
    python3 - "$REG" "$NAME" "$VER" "$1" <<'PY'
import hashlib, pathlib, struct, sys

reg, name, ver, state = sys.argv[1:5]
root = pathlib.Path(reg)

# A ustar member named app.wasm, enough for the loader to map.
body = b"app.wasm" + b"\0" * 504 + b"\0asm\x01\0\0\0" + b"\0" * 500
(root / f"{name}@{ver}.wapp").write_bytes(body)

stored = hashlib.sha256(body).digest()
if state == "digest_mismatch":
    stored = hashlib.sha256(body + b"tampered").digest()

flags = 0
key_id = 0
sig = b"\0" * 64
if state in ("unknown_key", "bad_signature"):
    flags = 0x02  # signed
    key_id = 9 if state == "unknown_key" else 1
    sig = b"\xa5" * 64
if state == "seeded":
    flags = 0x01

digests = stored + b"\0" * (32 * 3)
record = struct.pack("<IIIBBxx", 0x57415033, len(body), key_id, 1, flags)
record += digests + sig
assert len(record) == 208, len(record)

if state != "no_record":
    (root / f"{name}@{ver}.meta").write_bytes(record)
PY
}

run_engine() { # <enforce true|false> -> engine output
    local enforce=$1 cfg="$WORK/config.json"
    cat > "$cfg" <<JSON
{
  "system": { "privileged": true, "enforceImageVerify": $enforce },
  "supervisor": {
    "imagePath": "registry:$NAME:$VER",
    "params": { "console": { "in": { "name": "platform" }, "out": { "name": "platform" }, "err": { "name": "platform" } } }
  }
}
JSON
    ( cd "$WORK" && stdbuf -o0 -e0 timeout 5 "$PROJECT_ROOT/$WANTED" "$cfg" 2>&1 ) || true
}

check() { # <title> <state> <enforce> <expect-refusal yes|no>
    local title=$1 state=$2 enforce=$3 want=$4 out
    write_entry "$state"
    out=$(run_engine "$enforce")

    if echo "$out" | grep -q "refused: image verification $state"; then
        got=yes
    else
        got=no
    fi
    # The state is reported whether or not it refuses.
    if ! echo "$out" | grep -q "image verification.*$state"; then
        FAIL=$((FAIL + 1))
        printf '  FAIL  %s — the engine never reported %s\n' "$title" "$state"
        return
    fi
    if [ "$got" = "$want" ]; then
        PASS=$((PASS + 1))
        printf '  ok    %s\n' "$title"
    else
        FAIL=$((FAIL + 1))
        printf '  FAIL  %s — refusal expected %s, got %s\n' "$title" "$want" "$got"
    fi
}

echo "enforcing: an image that does not verify must not run"
check "tampered bytes are refused"        digest_mismatch true  yes
check "an unsigned image is refused"      no_signature    true  yes
check "an unheld key is refused"          unknown_key     true  yes
check "an entry with no record is refused" no_record      true  yes

echo "reporting: the same states are reported and the load proceeds"
check "tampered bytes are reported only"  digest_mismatch false no
check "an unsigned image is reported only" no_signature   false no

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
