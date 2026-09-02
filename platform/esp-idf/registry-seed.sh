#!/bin/bash
# Package the ESP-IDF factory-seed images: wapps/<name>/<name>.wasm becomes
# <out>/<name>.wapp, embedded into the firmware at configure time. The .wasm
# inputs come from `make wapps`; an absent one is reported, not built.
#
# An argument of the form <name>=<path> takes the .wasm from that path, which
# is how a board seeds a wapp built outside this repository.
#
# The filename is the image's identity, so an image seeded without a version
# has one. That is fine for a wapp a desired state names by tag, and wrong for
# one resolved by version: the supervisor asks for its installer at a pinned
# version, and an unversioned seed can never answer it.
#
# A wapp that must be resolvable by version ships `<name>.version` beside its
# wasm, written by the rule that builds it, and is seeded as
# `<name>@<version>.wapp`. Taking the version from that file rather than from
# an argument is what keeps the seeded ref from naming a version the image is
# not.
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
    # The version file sits beside the wasm, like a wapp's root/ does.
    ver_file="$(dirname "$src")/$n.version"
    ref="$n"
    if [ -f "$ver_file" ]; then
        ver="$(tr -d '[:space:]' < "$ver_file")"
        [ -n "$ver" ] && ref="$n@$ver"
    fi
    out="$OUT/$ref.wapp"

    # A version bump renames the image, so drop what the old name left behind
    # rather than seeding two versions of one wapp.
    for stale in "$OUT/$n.wapp" "$OUT/$n"@*.wapp; do
        [ -e "$stale" ] && [ "$stale" != "$out" ] && rm -f "$stale"
    done

    # Rewriting an up-to-date image would re-embed it and relink the firmware
    # on every build.
    if [ "$out" -nt "$src" ] &&
       { [ ! -d "$(dirname "$src")/root" ] ||
         [ -z "$(find "$(dirname "$src")/root" -newer "$out" -print -quit)" ]; }; then
        continue
    fi
    s="$(mktemp -d)"
    cp "$src" "$s/app.wasm"
    # A wapp that carries data files holds them in a root/ beside its wasm.
    if [ -d "$(dirname "$src")/root" ]; then
        cp -r "$(dirname "$src")/root/." "$s/"
    fi
    tar --format=ustar --owner=0 --group=0 --mtime='1970-01-01 00:00:00 UTC' \
        -C "$s" -cf "$out" .
    rm -rf "$s"
    echo "registry-seed: $out"
done
