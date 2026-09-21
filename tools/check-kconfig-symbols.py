#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flag CONFIG_WANTED_* references with no defining Kconfig entry.

An unknown symbol resolves to `n` with at most a warning from kconfiglib —
the feature silently disappears instead of failing the build. This finds
that class of bug cheaply, without evaluating Kconfig at all: it only
diffs the set of `config`/`menuconfig` names Kconfig* declares against the
set of CONFIG_WANTED_* identifiers referenced anywhere in first-party
source.

Usage: check-kconfig-symbols.py <repo-root>
"""
import re
import subprocess
import sys

KCONFIG_FILES = [
    "Kconfig",
    "Kconfig.target",
    "Kconfig.engine",
    "platform/esp-idf/components/wanted_engine/Kconfig",
]

DEF_RE = re.compile(r"^\s*(?:menu)?config\s+(WANTED_[A-Z0-9_]+)\b", re.MULTILINE)
CMACRO_DEF_RE = re.compile(r"^\s*#\s*define\s+CONFIG_(WANTED_[A-Z0-9_]+)\b", re.MULTILINE)
REF_RE = re.compile(r"\bCONFIG_(WANTED_[A-Z0-9_]+)\b")

SOURCE_GLOBS = [
    "*.c",
    "*.h",
    "*.cmake",
    "CMakeLists.txt",
    "Justfile",
    "Makefile",
    "*.sh",
]

PRUNE = [
    "third_party",
    "vendor",
    ".openwrt-sdk",
    "managed_components",
    "/.cache/",
    "build",
]


def defined_symbols(root):
    names = set()
    for rel in KCONFIG_FILES:
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        names.update(DEF_RE.findall(text))
    # A header may also derive a CONFIG_WANTED_* constant with #define,
    # from an actual Kconfig symbol (e.g. CONFIG_WANTED_MAX_WAPPS + 1) —
    # a legitimate defining site, not a reference to look up.
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "*.h"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    for rel in out:
        if any(p in rel for p in PRUNE):
            continue
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        names.update(CMACRO_DEF_RE.findall(text))
    return names


def referenced_symbols(root):
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        capture_output=True,
        text=True,
        check=True,
    ).stdout.splitlines()
    refs = {}
    for rel in out:
        if any(p in rel for p in PRUNE):
            continue
        if not any(rel.endswith(g.lstrip("*")) or rel.split("/")[-1] == g for g in SOURCE_GLOBS):
            continue
        try:
            text = (root / rel).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for m in REF_RE.finditer(text):
            name = m.group(1)
            # A trailing underscore means the match stopped at a shell/CMake
            # interpolation (${VARIANT}, etc.) — the real name is dynamic and
            # this grep cannot resolve it either way.
            if name.endswith("_"):
                continue
            refs.setdefault(name, []).append(rel)
    return refs


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    from pathlib import Path

    root = Path(argv[0])
    defined = defined_symbols(root)
    referenced = referenced_symbols(root)

    undefined = sorted(set(referenced) - defined)
    if not undefined:
        return 0
    for name in undefined:
        files = ", ".join(sorted(set(referenced[name]))[:3])
        print(f"CONFIG_{name}: referenced but no `config {name}` in any Kconfig* ({files})")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
