#!/bin/bash
# Resolve and validate CONFIG_WANTED_DEFAULT_CONFIG — every target ships the same
# file through here. Parsed now, because the alternative is a node that will not
# boot. Prints the resolved path; copies it to <dest> when given.
#
# Usage: default-config.sh <repo-root> <config-path> [dest]
set -euo pipefail

REPO="${1:?usage: default-config.sh <repo-root> <config-path> [dest]}"
CFG="${2:?usage: default-config.sh <repo-root> <config-path> [dest]}"
DEST="${3:-}"

case "$CFG" in /*) src="$CFG" ;; *) src="$REPO/$CFG" ;; esac

if [ ! -f "$src" ]; then
    echo "default-config: no such configuration: $src" >&2
    echo "  set one under 'Default configuration' in \`just menuconfig\`" >&2
    exit 1
fi

if ! python3 -m json.tool "$src" >/dev/null 2>&1; then
    echo "default-config: $src is not valid JSON" >&2
    python3 -m json.tool "$src" 2>&1 | sed 's/^/  /' >&2 || true
    exit 1
fi

if [ -n "$DEST" ]; then
    mkdir -p "$(dirname "$DEST")"
    cp "$src" "$DEST"
fi
printf '%s\n' "$src"
