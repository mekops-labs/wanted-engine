#!/usr/bin/env python3
"""Derive a board's AP-mode SSID and passphrase from its hardware serial.

Mirrors deriveApCredentials() in platform/esp-idf/vfs/vfs-wifi.c exactly, so
an operator can compute the credentials wifi-mgr's AP fallback will offer
before a board ever raises it. The serial is read once at bench/factory time
from /proc/wanted's `serial:` line (over the platform console, pre-
provisioning) and never leaves the device otherwise.
"""

import hashlib
import hmac
import re
import sys

SSID_PREFIX = "wanted-"
SSID_SUFFIX_LEN = 6  # hex digits: the low 24 bits of the serial
PASS_LABEL = b"wanted-ap-passphrase-v1"
PASS_HEX_LEN = 8  # first 4 HMAC bytes, rendered as hex — WPA2-PSK's own floor


def derive(serial: str) -> tuple[str, str]:
    ssid = SSID_PREFIX + serial[-SSID_SUFFIX_LEN:]
    digest = hmac.new(serial.encode("ascii"), PASS_LABEL, hashlib.sha256).digest()
    passphrase = digest[: PASS_HEX_LEN // 2].hex()
    return ssid, passphrase


def main():
    if len(sys.argv) != 2:
        print("usage: ap-credentials.py <serial>", file=sys.stderr)
        print("  <serial> is /proc/wanted's serial: line, verbatim", file=sys.stderr)
        return 2
    serial = sys.argv[1].strip()
    serial = re.sub(r"^serial:\s*", "", serial)  # a pasted /proc/wanted line
    if not re.fullmatch(r"[0-9a-f]+", serial):
        print(f"serial '{serial}' is not lowercase hex", file=sys.stderr)
        return 1
    ssid, passphrase = derive(serial)
    print(f"ssid:       {ssid}")
    print(f"passphrase: {passphrase}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
