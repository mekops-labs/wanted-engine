#!/bin/bash
# Report the WANTED engine's per-wapp and fixed memory footprint for each build
# profile, on both the host (LP64) and the 32-bit embedded (ILP32) ABI.
#
# Runs in the build container (clang + readelf); see `make sizes`. The figures
# come from a compile-only build of utils/measure_structs.c — no program is run
# and no target libc is needed, so the same source is measured for the 32-bit
# model via a freestanding cross-target. See that file's header for the trick.
#
# Definitions:
#   per-wapp footprint = per-wapp structs (wapp_t, slot, vfs/wasi/wamr context,
#                        log ring) + WASM stack + WASM app heap + worker thread
#                        native stack + one linear-memory page + approximate WAMR
#                        overhead.
#   engine overhead    = fixed boot/config structures (wantedConfig_t).
#   worst case         = engine overhead + MAX_WAPPS x per-wapp footprint.
#
# The struct and limit sizes are exact (measured from the real headers). The two
# WAMR-side addends are estimates so the total is a usable ballpark rather than a
# floor that ignores the runtime:
#   - LINEAR_FLOOR: one 64 KiB wasm page — the minimum linear memory an
#     instance reserves. A module that declares N initial pages uses N x this,
#     so scale it up for a memory-hungry wapp.
#   - WAMR_OVERHEAD: order-of-magnitude per-instance WAMR bookkeeping (module
#     instance, function/global/table instances, exec-env struct). Not exact;
#     grows with module complexity.
# Still excluded: the per-image writable module copy (wasm_bytes, = .wasm size).
set -euo pipefail

# WAMR per-instance bookkeeping — order-of-magnitude estimate (see header).
readonly WAMR_OVERHEAD=16384  # ~16 KiB
readonly WASM_PAGE=65536      # WASM linear-memory page

ENGINE_DIR=${ENGINE_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
SRC="$ENGINE_DIR/utils/measure_structs.c"
INC="-I$ENGINE_DIR/include -I$ENGINE_DIR/src/include -I$ENGINE_DIR/src/vfs \
     -I$ENGINE_DIR/vendor/cwalk/include -I$ENGINE_DIR/vendor/wamr/core/iwasm/include"
PROFILES=(tiny constrained small big)

# Freestanding 32-bit build needs only size_t from <stdlib.h>; stub it.
STUB=$(mktemp -d)
printf '#pragma once\n#include <stddef.h>\n' >"$STUB/stdlib.h"
trap 'rm -rf "$STUB"' EXIT

profile_defines() { # $1 = profile name
    sed -nE 's/^[[:space:]]*set\(([A-Z_]+)[[:space:]]+([0-9]+).*/-D\1=\2/p' \
        "$ENGINE_DIR/cmake/profiles/$1.cmake" | tr '\n' ' '
}

# WASM_MAX_MEMORY_PAGES for a profile (0 = uncapped). ABI-independent.
profile_cap() { # $1 = profile name
    local v
    v=$(sed -nE 's/^[[:space:]]*set\(WASM_MAX_MEMORY_PAGES[[:space:]]+([0-9]+).*/\1/p' \
        "$ENGINE_DIR/cmake/profiles/$1.cmake")
    echo "${v:-0}"
}

declare -A M
measure() { # $1 = abi (linux|nuttx), $2 = profile -> fills M[name]=size
    local abi="$1" defs obj res
    defs=$(profile_defines "$2")
    obj=$(mktemp --suffix=.o)
    if [ "$abi" = nuttx ]; then
        res=$(clang -print-resource-dir)
        clang -ffreestanding -target i386-unknown-linux-gnu -nostdinc \
            -I"$res/include" -I"$STUB" $INC $defs -fno-common -c "$SRC" -o "$obj"
    else
        clang $INC $defs -fno-common -c "$SRC" -o "$obj"
    fi
    # readelf prints the symbol Size in hex once it grows large; $(( )) folds
    # both hex and decimal to a plain decimal so downstream math/format is uniform.
    M=()
    while read -r sz name; do M[${name#measured_}]=$(( sz )); done \
        < <(readelf -sW "$obj" | awk '/measured_/{print $3, $8}')
    rm -f "$obj"
}

human() { # bytes -> human string
    awk -v b="$1" 'BEGIN{
        if (b < 1024) printf "%d B", b;
        else if (b < 1048576) printf "%.1f KB", b/1024;
        else printf "%.2f MB", b/1048576 }'
}

report_abi() { # $1 = abi label, $2 = abi key
    local abi="$2"
    measure "$abi" constrained
    printf '\n=== ABI: %s (sizeof(void*)=%s, sizeof(size_t)=%s) ===\n' \
        "$1" "${M[ptr]}" "${M[size_t]}"
    printf '%-12s %4s %9s %9s %10s %11s %10s %12s\n' \
        profile MAX structs wapp-mem max-linear per-wapp overhead "worst case"
    local p struct fixed over pages lin perw linstr perwstr worststr
    for p in "${PROFILES[@]}"; do
        measure "$abi" "$p"
        struct=$(( M[wapp_t] + M[wapp_data_t] + M[vfs_ctx_t] + M[wasi_ctx_t] \
                   + M[wamrData_t] + M[log_slot_t] ))
        fixed=$(( M[WASM_STACK_SIZE] + M[WASM_HEAP_SIZE] \
                  + M[WASM_WORKER_STACK_SIZE] + WAMR_OVERHEAD ))
        over=${M[wantedConfig_t]}
        pages=$(profile_cap "$p")
        if [ "$pages" -gt 0 ]; then
            lin=$(( pages * WASM_PAGE ))
            perw=$(( struct + fixed + lin ))
            linstr=$(human $lin); perwstr=$(human $perw)
            worststr=$(human $(( over + M[MAX_WAPPS] * perw )))
        else
            linstr="unbounded"; perwstr="unbounded"; worststr="unbounded"
        fi
        printf '%-12s %4s %9s %9s %10s %11s %10s %12s\n' \
            "$p" "${M[MAX_WAPPS]}" "$(human $struct)" "$(human $fixed)" \
            "$linstr" "$perwstr" "$(human $over)" "$worststr"
    done
}

echo "WANTED engine memory footprint by profile"
echo "structs    = exact per-wapp engine slot structures (wapp_t, vfs/wasi/wamr ctx, log ring)"
echo "wapp-mem   = per-wapp runtime memory: WASM stack + WASM app heap + worker native stack + ~16 KiB WAMR overhead (approx)"
echo "max-linear = WASM_MAX_MEMORY_PAGES x 64 KiB — the per-wapp linear-memory ceiling (unbounded = no cap)"
echo "per-wapp   = structs + wapp-mem + max-linear;  worst case = engine overhead + MAX_WAPPS x per-wapp"
echo "Excludes the per-image writable module copy."
report_abi "linux  (LP64)"  linux
report_abi "nuttx  (ILP32)" nuttx
