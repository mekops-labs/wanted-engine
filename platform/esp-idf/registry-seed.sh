#!/bin/bash
# Package the ESP-IDF factory-seed images: wapps/<name>/<name>.wasm becomes
# <out>/<name>.wapp, embedded into the firmware at configure time. The .wasm
# inputs come from `make wapps`; an absent one is reported, not built.
#
# An argument of the form <name>=<path> takes the .wasm from that path, which
# is how a board seeds a wapp built outside this repository.
#
# Usage: registry-seed.sh <repo-root> <out-dir> <name>[=<path>]...
set -euo pipefail

REPO="${1:?usage: registry-seed.sh <repo-root> <out-dir> <name>...}"
OUT="${2:?usage: registry-seed.sh <repo-root> <out-dir> <name>...}"
shift 2

mkdir -p "$OUT"
for a in "$@"; do
    case "$a" in
        *=*) n="${a%%=*}"; src="${a#*=}" ;;
        *)   n="$a"; src="$REPO/wapps/$n/$n.wasm" ;;
    esac
    if [ ! -f "$src" ]; then
        echo "registry-seed: $src missing — run 'make wapps' first" >&2
        exit 1
    fi
    # Rewriting an up-to-date image would re-embed it and relink the firmware
    # on every build.
    if [ "$OUT/$n.wapp" -nt "$src" ]; then
        continue
    fi
    s="$(mktemp -d)"
    cp "$src" "$s/app.wasm"
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$OUT/$n.wapp" app.wasm
    rm -rf "$s"
    echo "registry-seed: $OUT/$n.wapp"
done
