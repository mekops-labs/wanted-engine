#!/bin/bash
# Report the WANTED engine's per-wapp and fixed memory footprint for every
# defconfig in configs/, on both the host (LP64) and 32-bit embedded (ILP32) ABI.
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
KCL="$ENGINE_DIR/tools/kconfiglib"

# Two modes: the full survey, and this build dir alone — the latter is what the
# configuration recipes run after writing a .config.
MODE="${1:-all}"
case "$MODE" in
    all|current) ;;
    *) echo "usage: measure-sizes.sh [all|current]" >&2; exit 1 ;;
esac

# The build dir's own .config, named for the dir so two stay distinguishable.
BUILD_DIR=${BUILD_DIR:-build}
CURRENT_CONFIG="$ENGINE_DIR/$BUILD_DIR/.config"
CURRENT_NAME="$BUILD_DIR/.config"

if [ "$MODE" = current ]; then
    [ -f "$CURRENT_CONFIG" ] || {
        echo "measure-sizes: no configuration at $CURRENT_NAME" >&2; exit 1; }
    PROFILES=("$CURRENT_NAME")
else
    # Discovered, not listed, so a new board defconfig needs no edit here.
    # Capacity envelopes ascending, then boards alphabetically, then this dir.
    mapfile -t PROFILES < <(
        for f in "$ENGINE_DIR"/configs/*_defconfig; do
            n=$(basename "$f"); n=${n%_defconfig}
            case "$n" in
                tiny)        echo "0 $n" ;;
                constrained) echo "1 $n" ;;
                small)       echo "2 $n" ;;
                big)         echo "3 $n" ;;
                *)           echo "4 $n" ;;
            esac
        done | sort -k1,1 -k2,2 | cut -d" " -f2
    )
    [ "${#PROFILES[@]}" -gt 0 ] || { echo "no configs/*_defconfig found" >&2; exit 1; }
    if [ -f "$CURRENT_CONFIG" ]; then
        PROFILES+=("$CURRENT_NAME")
    fi
fi

# Widest name, so the table stays aligned as board defconfigs come and go.
NAMEW=8
for _p in "${PROFILES[@]}"; do [ "${#_p}" -gt "$NAMEW" ] && NAMEW=${#_p}; done

# Freestanding 32-bit build needs only size_t from <stdlib.h>; stub it.
STUB=$(mktemp -d)
printf '#pragma once\n#include <stddef.h>\n' >"$STUB/stdlib.h"
trap 'rm -rf "$STUB"' EXIT

# Generate wanted-autoconf.h for an envelope into its own dir, and echo the dir.
# The engine headers include that header, so this is what makes the measured
# limits the configured ones rather than a second copy of the numbers.
profile_include() { # $1 = profile name -> stdout: dir holding wanted-autoconf.h
    local d="$STUB/cfg-${1//\//_}"
    [ -f "$d/wanted-autoconf.h" ] && { echo "$d"; return; }
    mkdir -p "$d"
    ( cd "$ENGINE_DIR"
      if [ "$1" = "$CURRENT_NAME" ]; then
          # Already a .config; generating from Kconfig.engine drops the target
          # half, the same narrowing every engine build does.
          cp "$CURRENT_CONFIG" "$d/.config"
      else
          PYTHONPATH="$KCL" KCONFIG_CONFIG="$d/.config" \
            python3 "$KCL/defconfig.py" --kconfig Kconfig.engine "configs/$1_defconfig" \
            >/dev/null 2>&1
      fi
      PYTHONPATH="$KCL" KCONFIG_CONFIG="$d/.config" \
        python3 "$KCL/genconfig.py" --header-path "$d/wanted-autoconf.h" Kconfig.engine \
        >/dev/null 2>&1 )
    # Silenced above (kconfiglib is noisy about the narrowing), so check here:
    # a failure would otherwise surface as an unexplained clang error.
    [ -f "$d/wanted-autoconf.h" ] || {
        echo "measure-sizes: could not generate a header for '$1'" >&2; exit 1; }
    echo "$d"
}

# WASM_MAX_MEMORY_PAGES as configured, read from .config so 0 (uncapped) stays
# distinguishable from 1 — EMIT floors symbol sizes at 1 byte.
profile_cap() { # $1 = profile name
    local v
    v=$(sed -nE 's/^CONFIG_WANTED_WASM_MAX_MEMORY_PAGES=([0-9]+)$/\1/p' \
        "$(profile_include "$1")/.config")
    echo "${v:-0}"
}

declare -A M
# SECURE_SOCKETS is a build-system define rather than a configuration symbol,
# so derive it from the same .config: the engine headers refuse to compile if
# the two disagree.
profile_tls() { # $1 = profile name -> 1 | 0
    grep -q '^CONFIG_WANTED_VFS_SOCKET_TLS=y$' "$(profile_include "$1")/.config" \
        && echo 1 || echo 0
}

measure() { # $1 = abi (linux|nuttx), $2 = profile -> fills M[name]=size
    local abi="$1" cfg obj res tls
    cfg=$(profile_include "$2")
    tls=$(profile_tls "$2")
    obj=$(mktemp --suffix=.o)
    if [ "$abi" = nuttx ]; then
        res=$(clang -print-resource-dir)
        clang -ffreestanding -target i386-unknown-linux-gnu -nostdinc \
            -I"$res/include" -I"$STUB" -I"$cfg" $INC -DSECURE_SOCKETS="$tls" \
            -fno-common -c "$SRC" -o "$obj"
    else
        clang -I"$cfg" $INC -DSECURE_SOCKETS="$tls" -fno-common -c "$SRC" -o "$obj"
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

# Which ABI the configured target builds for. Only the build dir's .config can
# answer it — the defconfigs carry no target, which is why the survey shows both.
# Sets ABI_KEY (linux | nuttx | empty) and ABI_WHY; empty is a real answer, not
# a failure, since a custom SDK URL need not name an architecture.
ABI_KEY=""; ABI_WHY=""

# Resolve an SDK argument to an ABI from an extracted SDK's toolchain triple.
# Returns 1 when none is on disk, leaving the caller to fall back to the name.
openwrt_sdk_abi() { # $1 = SDK keyword | URL | directory
    local dir base tc
    if [ -d "$1" ]; then
        dir="$1"
    else
        # Same cache layout sdk-env.sh writes.
        base="$(basename "$1")"
        dir="$ENGINE_DIR/.openwrt-sdk/${base%.tar.*}"
    fi
    tc="$(ls -d "$dir"/staging_dir/toolchain-* 2>/dev/null | head -1)" || return 1
    [ -n "$tc" ] || return 1
    case "$(basename "$tc")" in
        *aarch64*)       ABI_KEY=linux ;;
        *mips*|*arm_*|*riscv32*|*i386*) ABI_KEY=nuttx ;;
        *x86_64*)        ABI_KEY=linux ;;
        *) return 1 ;;
    esac
    ABI_WHY="OpenWrt SDK $(basename "$tc") — $([ "$ABI_KEY" = linux ] && echo 64-bit || echo 32-bit)"
    return 0
}

detect_abi() { # $1 = .config path
    local cfg="$1" target board sdk
    target=$(sed -nE 's/^CONFIG_WANTED_TARGET="(.*)"$/\1/p' "$cfg")
    case "$target" in
        linux)
            ABI_KEY=linux; ABI_WHY="target linux — the build host" ;;
        esp-idf)
            ABI_KEY=nuttx; ABI_WHY="target esp-idf — 32-bit xtensa/riscv" ;;
        nuttx)
            board=$(sed -nE 's/^CONFIG_WANTED_TARGET_NUTTX_BOARD="(.*)"$/\1/p' "$cfg")
            case "$board" in
                sim:*)          ABI_KEY=linux; ABI_WHY="NuttX board '$board' — the sim runs on the host" ;;
                esp32*|rp2350*) ABI_KEY=nuttx; ABI_WHY="NuttX board '$board' — 32-bit" ;;
                *)              ABI_KEY="";    ABI_WHY="NuttX board '$board' — architecture not recognised" ;;
            esac ;;
        openwrt)
            sdk=$(sed -nE 's/^CONFIG_WANTED_TARGET_OPENWRT_SDK="(.*)"$/\1/p' "$cfg")
            # The toolchain triple is the only answer that is not a guess.
            # Cache is read, never fetched: this runs after every menuconfig.
            if ! openwrt_sdk_abi "$sdk"; then
                case "$sdk" in
                    aarch64|*aarch64*|*armv8*)  ABI_KEY=linux; ABI_WHY="OpenWrt SDK '$sdk' — 64-bit (from the name)" ;;
                    mipsel|*mipsel*|*malta*)    ABI_KEY=nuttx; ABI_WHY="OpenWrt SDK '$sdk' — 32-bit (from the name)" ;;
                    *)                          ABI_KEY="";    ABI_WHY="OpenWrt SDK '$sdk' — architecture not in the name, and no extracted SDK to ask" ;;
                esac
            fi ;;
        *)  ABI_KEY=""; ABI_WHY="no target selected" ;;
    esac
}

report_abi() { # $1 = abi label, $2 = abi key
    local abi="$2"
    # Pointer width is an ABI property, so any reported configuration answers it.
    measure "$abi" "${PROFILES[0]}"
    printf '\n=== ABI: %s (sizeof(void*)=%s, sizeof(size_t)=%s) ===\n' \
        "$1" "${M[ptr]}" "${M[size_t]}"
    printf "%-${NAMEW}s %4s %9s %9s %10s %11s %10s %12s\n" \
        envelope MAX structs wapp-mem max-linear per-wapp overhead "worst case"
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
        printf "%-${NAMEW}s %4s %9s %9s %10s %11s %10s %12s\n" \
            "$p" "${M[MAX_WAPPS]}" "$(human $struct)" "$(human $fixed)" \
            "$linstr" "$perwstr" "$(human $over)" "$worststr"
    done
}

if [ "$MODE" = current ]; then
    # Run after every configuration change, so it stays short.
    echo "WANTED engine memory footprint for $CURRENT_NAME"
    detect_abi "$CURRENT_CONFIG"
    if [ -n "$ABI_KEY" ]; then
        echo "$ABI_WHY"
        case "$ABI_KEY" in
            linux) report_abi "linux  (LP64)"  linux ;;
            nuttx) report_abi "nuttx  (ILP32)" nuttx ;;
        esac
        exit 0
    fi
    echo "$ABI_WHY — showing both ABIs"
else
    echo "WANTED engine memory footprint by resource envelope"
    echo "structs    = exact per-wapp engine slot structures (wapp_t, vfs/wasi/wamr ctx, log ring)"
    echo "wapp-mem   = per-wapp runtime memory: WASM stack + WASM app heap + worker native stack + ~16 KiB WAMR overhead (approx)"
    echo "max-linear = CONFIG_WANTED_WASM_MAX_MEMORY_PAGES x 64 KiB — the per-wapp linear-memory ceiling (unbounded = no cap)"
    echo "per-wapp   = structs + wapp-mem + max-linear;  worst case = engine overhead + MAX_WAPPS x per-wapp"
    echo "Excludes the per-image writable module copy."
    if [ ! -f "$CURRENT_CONFIG" ]; then
        echo "($CURRENT_NAME not present — configure a build dir to see it reported here)"
    fi
fi
report_abi "linux  (LP64)"  linux
report_abi "nuttx  (ILP32)" nuttx
