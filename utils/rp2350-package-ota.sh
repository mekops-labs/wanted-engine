#!/bin/bash
# Extract the raw, unwrapped app image an RP2350 board's OTA slot expects.
#
# `picotool uf2 convert` and the SWD/BOOTSEL flash targets all consume the ELF
# or the UF2 — both are flash-offset-aware or debugger formats, neither is the
# bare byte stream `/dev/ota/slot` wants (the ESP-IDF targets keep the same
# split: a merged, flashable-at-offset-0 image for a first flash, plus a bare
# `-ota.bin` app image for the update path — see `idf.py merge-bin` /
# `wanted-$chip-ota.bin` in the Justfile). objcopy is what produces that here;
# `picotool uf2 convert` has no raw-bin output mode to substitute.
#
# Usage: utils/rp2350-package-ota.sh <elf> <board> <dist-dir>
#   utils/rp2350-package-ota.sh third_party/nuttx/nuttx pimoroni-pico-2-plus-w dist/nuttx
set -euo pipefail

[ $# -eq 3 ] || {
    echo "usage: $0 <elf> <board> <dist-dir>" >&2
    exit 2
}

elf=$1
board=$2
dist=$3

[ -f "$elf" ] || {
    echo "FAIL: elf not found: $elf" >&2
    exit 1
}

command -v arm-none-eabi-objcopy >/dev/null 2>&1 || {
    echo "FAIL: arm-none-eabi-objcopy not on PATH — run this inside the RP2350 toolchain image" >&2
    exit 1
}

mkdir -p "$dist"
out="$dist/$board-ota.bin"
arm-none-eabi-objcopy -O binary "$elf" "$out"

echo "==> dist: $out ($(stat -c %s "$out") bytes, sha256:$(sha256sum "$out" | cut -d' ' -f1))"
