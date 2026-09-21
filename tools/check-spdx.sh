#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Checks that each given file carries the SPDX header on the line it belongs
# on: line 1 for C/H sources, line 2 (after the shebang) for shell scripts.
#
# Usage: check-spdx.sh <file>...
set -euo pipefail

want='SPDX-License-Identifier: Apache-2.0'
fail=0

for f in "$@"; do
    case "$f" in
        *.c | *.h)
            line=$(sed -n '1p' "$f")
            ;;
        *.sh)
            line=$(sed -n '2p' "$f")
            ;;
        *)
            continue
            ;;
    esac
    case "$line" in
        *"$want"*) ;;
        *)
            echo "$f: missing '$want' header" >&2
            fail=1
            ;;
    esac
done

exit $fail
