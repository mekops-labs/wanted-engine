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
# --keyed adds the states that need a key the firmware holds. The engine must
# then be built from configs/imageverify_defconfig.
KEYED=${2:-}
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
# The signer's published test vector: a fixed seed whose public half the
# imageverify profile compiles in. It secures nothing.
KEY_SEED=0101010101010101010101010101010101010101010101010101010101010101

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

# sign_for <name> <version> — sign the message that identity and this entry's
# bytes build, and put it in the entry's record.
sign_for() {
    python3 - "$REG" "$NAME" "$VER" "$1" "$2" "$KEY_SEED" "$WORK" <<'SIGN'
import hashlib, pathlib, struct, subprocess, sys

reg, name, ver, as_name, as_ver, seed, work = sys.argv[1:8]
root = pathlib.Path(reg)
body = (root / f"{name}@{ver}.wapp").read_bytes()
digest = hashlib.sha256(body).digest()

msg = bytes([len(as_name)]) + as_name.encode()
msg += bytes([len(as_ver)]) + as_ver.encode()
msg += bytes([1]) + digest

# An Ed25519 key from the fixed seed, in the PKCS#8 shape openssl reads.
der = bytes.fromhex("302e020100300506032b657004220420" + seed)
key = pathlib.Path(work) / "key.pem"
subprocess.run(["openssl", "pkey", "-inform", "DER", "-out", str(key)],
               input=der, check=True, capture_output=True)
msg_file = pathlib.Path(work) / "msg.bin"
msg_file.write_bytes(msg)
sig = subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(key),
                      "-in", str(msg_file)], check=True, capture_output=True).stdout

rec = root / f"{name}@{ver}.meta"
record = bytearray(rec.read_bytes())
struct.pack_into("<I", record, 8, 1)   # key id 1, which the profile holds
record[13] = 0x02                       # signed
record[16 + 128:16 + 128 + 64] = sig
rec.write_bytes(bytes(record))
SIGN
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

if [ "$KEYED" = "--keyed" ]; then
    echo "keyed: the states that need a key the firmware holds"

    keyed_case() { # <title> <sign-as-name> <sign-as-version> <expect ok|refused>
        local title=$1 as_name=$2 as_ver=$3 want=$4 out
        write_entry ok
        sign_for "$as_name" "$as_ver"
        out=$(run_engine true)
        if echo "$out" | grep -q "refused: image verification bad_signature"; then
            got=refused
        elif echo "$out" | grep -q "refused: image verification"; then
            got=other
        else
            got=ok
        fi
        if [ "$got" = "$want" ]; then
            PASS=$((PASS + 1))
            printf '  ok    %s\n' "$title"
        else
            FAIL=$((FAIL + 1))
            printf '  FAIL  %s — wanted %s, got %s\n' "$title" "$want" "$got"
            echo "$out" | grep "image verification" | tail -2
        fi
    }

    keyed_case "a correctly signed image loads"          "$NAME"  "$VER" ok
    keyed_case "bytes signed as another wapp are refused" imposter "$VER" refused
    keyed_case "bytes signed as another version are refused" "$NAME" 9.9.9 refused
fi

printf '\n%d passed, %d failed\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]
