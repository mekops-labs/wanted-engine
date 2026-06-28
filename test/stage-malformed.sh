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

# tar the contents of $1 into the registry as <$2>@<ver>.wapp
pack() {
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$1" -cf "$REG/$2@$VER.wapp" $(cd "$1" && ls)
}

mkdir -p "$REG"

# no entrypoint — a payload file but no app.wasm
s=$(mktemp -d); printf 'no entrypoint here' >"$s/payload.txt"; pack "$s" noappwasm; rm -rf "$s"

# app.wasm is not valid WebAssembly
s=$(mktemp -d); head -c 64 /dev/urandom >"$s/app.wasm"; pack "$s" badwasm; rm -rf "$s"

# truncated archive — a valid image cut short mid-stream
s=$(mktemp -d); cp "$GOOD" "$s/app.wasm"
tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
    -C "$s" -cf "$s/full.tar" app.wasm
head -c 600 "$s/full.tar" >"$REG/truncated@$VER.wapp"; rm -rf "$s"
