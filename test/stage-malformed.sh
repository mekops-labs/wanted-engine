#!/bin/bash
# Stage malformed registry images for the loader-robustness checks. The selftest
# supervisor tries to `start` each and asserts the engine rejects it without
# crashing. These are hand-crafted (not built from a wapp source) to exercise
# the loader's rejection paths.
#
# Usage: stage-malformed.sh <registry-dir> <path-to-a-valid-app.wasm>
set -u

REG=$1
GOOD=$2
VER=0.0.1-1

manifest() { printf '{"name":"%s","version":[0,0,1],"package":1,"requirements":[]}' "$1"; }

# tar the contents of $1 into the registry as <$2>:<ver>.wapp
pack() {
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$1" -cf "$REG/$2:$VER.wapp" $(cd "$1" && ls)
}

mkdir -p "$REG"

# missing manifest.json — only an app.wasm
s=$(mktemp -d); cp "$GOOD" "$s/app.wasm"; pack "$s" nomanifest; rm -rf "$s"

# missing app.wasm — only a manifest
s=$(mktemp -d); manifest noappwasm >"$s/manifest.json"; pack "$s" noappwasm; rm -rf "$s"

# app.wasm is not valid WebAssembly
s=$(mktemp -d); head -c 64 /dev/urandom >"$s/app.wasm"; manifest badwasm >"$s/manifest.json"
pack "$s" badwasm; rm -rf "$s"

# manifest.json is not valid JSON
s=$(mktemp -d); cp "$GOOD" "$s/app.wasm"; printf 'this is not json' >"$s/manifest.json"
pack "$s" badmanifest; rm -rf "$s"

# truncated archive — a valid image cut short mid-stream
s=$(mktemp -d); cp "$GOOD" "$s/app.wasm"; manifest truncated >"$s/manifest.json"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$s" -cf "$s/full.tar" app.wasm manifest.json
head -c 600 "$s/full.tar" >"$REG/truncated:$VER.wapp"; rm -rf "$s"
