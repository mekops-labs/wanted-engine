#!/bin/bash
# Compile the configured launch config into a C header: the CLI's no-argument
# default, and the only config a target without a filesystem has at boot. xxd
# rather than a string literal, which is a quoting bug waiting to happen.
#
# Usage: default-config-header.sh <repo-root> <config-path> <out-header>
set -euo pipefail

REPO="${1:?usage: default-config-header.sh <repo-root> <config-path> <out-header>}"
CFG="${2:?usage: default-config-header.sh <repo-root> <config-path> <out-header>}"
OUT="${3:?usage: default-config-header.sh <repo-root> <config-path> <out-header>}"

src="$("$REPO/utils/default-config.sh" "$REPO" "$CFG")"

mkdir -p "$(dirname "$OUT")"
{
    echo "/* Generated from $(basename "$src") — do not edit. */"
    echo "static const char wantedDefaultConfig[] = {"
    xxd -i < "$src"
    echo "  , 0x00 /* consumed as a C string */"
    echo "};"
} > "$OUT.tmp"

# Replace only on change; a fresh timestamp rebuilds everything including it.
if cmp -s "$OUT.tmp" "$OUT"; then
    rm -f "$OUT.tmp"
else
    mv "$OUT.tmp" "$OUT"
fi
