#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flag comment blocks over the line budget, and stale edit-history notes.
By default checks only lines added in the staged diff (for the pre-commit
hook); --all scans whole files instead, for a repo-wide audit.
Usage: check-comments.py [--all] <file>...
"""
import re
import subprocess
import sys

MAX_LINES = 3

STALE_PATTERNS = [
    re.compile(r"\bwas\b.*\bnow\b", re.IGNORECASE),
    re.compile(r"\bused to\b.*\bnow\b", re.IGNORECASE),
    re.compile(r"\binstead of the old\b", re.IGNORECASE),
    re.compile(r"\brather than the old\b", re.IGNORECASE),
    re.compile(r"\bno longer\b", re.IGNORECASE),
    re.compile(r"\bremoved:\s", re.IGNORECASE),
    re.compile(r"\bfixed to\b", re.IGNORECASE),
    re.compile(r"\bchanged to\b", re.IGNORECASE),
    re.compile(r"\blegacy\b", re.IGNORECASE),
]


def added_lines(path):
    """(lineno, text) for lines this file adds in the staged diff."""
    diff = subprocess.run(
        ["git", "diff", "--staged", "--unified=0", "--", path],
        capture_output=True,
        text=True,
        check=False,
    ).stdout
    lineno = None
    for line in diff.splitlines():
        if line.startswith("@@"):
            m = re.search(r"\+(\d+)", line)
            lineno = int(m.group(1)) if m else None
            continue
        if line.startswith("+++") or line.startswith("---"):
            continue
        if line.startswith("+"):
            yield lineno, line[1:]
            lineno += 1
        elif not line.startswith("-"):
            lineno = (lineno or 0) + 1


def all_lines(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f, 1):
            yield i, line.rstrip("\n")


def scan_comments(source):
    """Yield (start_line, end_line, text_lines) for each comment in file
    order: a full /* ... */ token by its own span, or a run of adjacent
    standalone // lines merged into one block."""
    lines = list(source)
    by_no = dict(lines)
    if not by_no:
        return
    nos = sorted(by_no)
    in_block = False
    block_start = None
    block_text = []
    slash_start = None
    slash_text = []
    prev_no = None
    found = []

    def flush_slash():
        nonlocal slash_start, slash_text
        if slash_start is not None:
            found.append((slash_start, prev_no, slash_text))
        slash_start = None
        slash_text = []

    for no in nos:
        text = by_no[no]
        stripped = text.strip()
        contiguous = prev_no is not None and no == prev_no + 1
        if in_block:
            block_text.append(text)
            if "*/" in text:
                found.append((block_start, no, block_text))
                in_block = False
                block_text = []
            prev_no = no
            continue
        if not contiguous:
            flush_slash()
        if stripped.startswith("//"):
            if slash_start is None:
                slash_start = no
            slash_text.append(text)
        else:
            flush_slash()
            if stripped.startswith("/*"):
                if "*/" in stripped[2:]:
                    found.append((no, no, [text]))
                else:
                    in_block = True
                    block_start = no
                    block_text = [text]
        prev_no = no
    flush_slash()
    found.sort()
    yield from found


def check_file(path, source):
    failures = []
    for start, end, text_lines in scan_comments(source):
        n = end - start + 1
        if n > MAX_LINES:
            failures.append(f"{path}:{start}: comment block is {n} lines (max {MAX_LINES})")
        joined = " ".join(t.strip() for t in text_lines)
        for pat in STALE_PATTERNS:
            if pat.search(joined):
                failures.append(f"{path}:{start}: reads like a stale edit-history note ({pat.pattern!r})")
                break
    return failures


def main(argv):
    full = "--all" in argv
    paths = [a for a in argv if a != "--all"]
    if not paths:
        print(__doc__)
        return 2
    source_fn = all_lines if full else added_lines
    failures = []
    for path in paths:
        failures.extend(check_file(path, source_fn(path)))
    for f in failures:
        print(f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
