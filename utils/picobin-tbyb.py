#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0

"""Mark a firmware image try-before-you-buy.

Sets the TBYB bit in the image's IMAGE_DEF, so the loader boots it
provisionally and reverts on the next reset unless the running firmware
confirms it. `picotool seal` has no option for this bit, so it is set here
and the image is sealed afterwards, leaving the hash to cover it.

Usage: picobin-tbyb.py <infile.uf2|infile.bin> <outfile.bin>

Takes the build's UF2 or a raw image and writes a raw image, since sealing
reads one. Run before sealing; sealing an already-sealed image is not what
this is for. Verify the result with `picotool info`, which reports `tbyb`
even though it cannot set it.
"""

import struct
import sys

# A picobin block opens with this marker.
BLOCK_MARKER_START = 0xFFFFDED3

# Item header: type in the low byte, size in words in the next, and for the
# image-type item a 16-bit value in the top half.
ITEM_IMAGE_TYPE = 0x42
ITEM_LAST = 0x7F | 0x80
ITEM_IGNORED = 0x7E | 0x80

# Bit 15 of the image-type value: boot this image provisionally.
IMAGE_TYPE_TBYB = 0x8000

# The block sits early in the image; the linker emits it in the first 4 KiB.
BLOCK_SEARCH_BYTES = 8192
MAX_ITEMS = 32

# UF2 carries 256 payload bytes in each 512-byte block, behind two magics.
UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_BLOCK_SIZE = 512
UF2_HEADER_WORDS = 8


def unwrap_uf2(data: bytes) -> bytearray:
    """Payload of a UF2, in block order, or the input if it is not one."""
    if len(data) < UF2_BLOCK_SIZE or struct.unpack_from("<I", data, 0)[0] != (
        UF2_MAGIC_START0
    ):
        return bytearray(data)
    if len(data) % UF2_BLOCK_SIZE:
        raise SystemExit("UF2 length is not a multiple of the block size")

    out = bytearray()
    expect = None
    for off in range(0, len(data), UF2_BLOCK_SIZE):
        magic0, magic1, _flags, addr, size = struct.unpack_from(
            "<5I", data, off
        )
        if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1:
            raise SystemExit(f"bad UF2 magic at {off:#x}")
        if expect is not None and addr != expect:
            raise SystemExit(f"UF2 is not contiguous at {addr:#x}")
        header = UF2_HEADER_WORDS * 4
        out += data[off + header : off + header + size]
        expect = addr + size
    return out


def set_tbyb(data: bytearray) -> int:
    """Set the TBYB bit, returning the offset of the word it changed."""
    for off in range(0, min(BLOCK_SEARCH_BYTES, len(data) - 4), 4):
        if struct.unpack_from("<I", data, off)[0] != BLOCK_MARKER_START:
            continue

        pos = off + 4
        for _ in range(MAX_ITEMS):
            (header,) = struct.unpack_from("<I", data, pos)
            item = header & 0xFF
            words = (header >> 8) & 0xFF

            if item == ITEM_IMAGE_TYPE:
                value = (header >> 16) & 0xFFFF
                if value & IMAGE_TYPE_TBYB:
                    raise SystemExit("image is already try-before-you-buy")
                value |= IMAGE_TYPE_TBYB
                struct.pack_into(
                    "<I", data, pos, (header & 0x0000FFFF) | (value << 16)
                )
                return pos

            if item in (ITEM_LAST, ITEM_IGNORED) or words == 0:
                break
            pos += words * 4

        raise SystemExit("no image-type item in the first block")

    raise SystemExit("no picobin block found")


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit(f"usage: {sys.argv[0]} <infile.bin> <outfile.bin>")

    with open(sys.argv[1], "rb") as f:
        data = unwrap_uf2(f.read())

    offset = set_tbyb(data)

    with open(sys.argv[2], "wb") as f:
        f.write(bytes(data))

    print(f"try-before-you-buy set at {offset:#x} -> {sys.argv[2]}")


if __name__ == "__main__":
    main()
