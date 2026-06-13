#!/bin/bash
# Run the in-WASM selftest supervisor and check its TAP output.
#
# The selftest supervisor (wasm/supervisor/selftest) boots, asserts the engine
# from inside WASM, launches the test wapp and checks the
# engine contains it, and prints TAP to stdout (the platform console). This
# runner stages the launched test wapps into the registry, boots the engine,
# and checks the TAP: a plan line and no `not ok`.
#
# Usage: test/selftest.sh [wanted-cli] [config]
set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
cd "$ROOT"

WANTED=${1:-./build/cmd/wanted-cli}
CONFIG=${2:-./test/selftest-config.json}
REGISTRY_ROOT=${REGISTRY_ROOT:-./registry}

if [ ! -x "$WANTED" ]; then
    echo "FAIL: wanted-cli not found at $WANTED (run 'make build')"
    exit 1
fi

mkdir -p "$REGISTRY_ROOT"

# Launched test wapps, packaged into the registry as <name>:<version>.wapp.
TEST_WAPPS="trapper:0.0.1-1 looper:0.0.1-1 stackbomb:0.0.1-1 membomb:0.0.1-1 cpuhog:0.0.1-1 blocker:0.0.1-1 pblock:0.0.1-1 escaper:0.0.1-1 fdhog:0.0.1-1 crasher:0.0.1-1 argenv:0.0.1-1"

staged=""
# stage <name>:<ver> [srcdir]
# Package wapps/<srcdir> (default <srcdir>=<name>) into the registry as
# <name>:<ver>.wapp. When an alias name differs from the source dir, the
# manifest is synthesised carrying the registry name — the engine reports a
# wapp under its manifest name, so an alias must own that name (this is how the
# single `duplex` source is staged as both `reader` and `writer`).
stage() {
    local name=${1%%:*} ver=${1#*:} src=${2:-${1%%:*}}
    make -C "wapps/$src" >/dev/null 2>&1 || { echo "FAIL: build wapps/$src"; exit 1; }
    local s img
    s=$(mktemp -d)
    img="$REGISTRY_ROOT/$name:$ver.wapp"
    cp "wapps/$src/$src.wasm" "$s/app.wasm"
    if [ "$src" = "$name" ]; then
        cp "wapps/$src/manifest.json" "$s/manifest.json"
    else
        local vv=${ver%-*} pkg=${ver#*-}
        local mj=${vv%%.*}
        local rest=${vv#*.}
        local mn=${rest%%.*}
        local pt=${rest##*.}
        printf '{"name":"%s","version":[%s,%s,%s],"package":%s,"requirements":[]}\n' \
            "$name" "$mj" "$mn" "$pt" "$pkg" > "$s/manifest.json"
    fi
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$img" app.wasm manifest.json
    rm -rf "$s"
    staged="$staged $img"
}
malformed="nomanifest noappwasm badwasm badmanifest truncated"
cleanup() {
    rm -f $staged
    for m in $malformed; do rm -f "$REGISTRY_ROOT/$m":*.wapp; done
}
trap cleanup EXIT

for w in $TEST_WAPPS; do stage "$w"; done

# The inter-wapp pipe round-trip uses one source (wapps/duplex) staged under two
# names; each picks its side from the ROLE env var in its launch config.
stage "reader:0.0.1-1" duplex
stage "writer:0.0.1-1" duplex

# Hand-crafted malformed images for the loader-robustness check (reuse a valid
# wasm that the loop above already built).
./test/stage-malformed.sh "$REGISTRY_ROOT" wapps/trapper/trapper.wasm

# The engine keeps running after the test supervisor finishes (a cleanly exited
# supervisor is respawned), so run it in the background and stop as soon as the
# TAP plan line appears rather than waiting out a timeout. tail -f streams the
# console live so the TAP and phase lines appear as they happen instead of all at
# once at the end.
LOG=$(mktemp)
"$WANTED" "$CONFIG" >"$LOG" 2>&1 &
WPID=$!
tail -n +1 -f "$LOG" 2>/dev/null &
TAILPID=$!
for _ in $(seq 1 90); do
    grep -qE '^1\.\.' "$LOG" 2>/dev/null && break
    kill -0 "$WPID" 2>/dev/null || break
    sleep 1
done
kill "$WPID" 2>/dev/null || true
wait "$WPID" 2>/dev/null || true
sleep 1                      # let tail flush the final lines
kill "$TAILPID" 2>/dev/null || true
wait "$TAILPID" 2>/dev/null || true
OUT=$(cat "$LOG"); rm -f "$LOG"

plan=$(printf '%s\n' "$OUT" | grep -E '^1\.\.[0-9]+' | head -1)
if [ -z "$plan" ]; then
    echo "FAIL: no TAP plan line (supervisor did not finish)"
    exit 1
fi
if printf '%s\n' "$OUT" | grep -q '^not ok'; then
    echo "FAIL: one or more checks failed"
    exit 1
fi
echo "PASS: selftest ($plan)"
exit 0
