#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the Conventional Commits shape this repo actually uses:
`type(scope)?!?: description`, type/scope free-form (a subsystem name is as
valid as feat/fix/docs/...). A body, if present, is `- one bullet per line`,
80 chars max per line. No Co-Authored-By trailer. Merge commits are exempt.
Usage: check-commit-msg.py <msg-file>
"""
import re
import sys

SUBJECT_RE = re.compile(r"^[a-z][a-z0-9/+,-]*(\([a-z0-9_,. -]+\))?!?: .+$")
MERGE_RE = re.compile(r"^Merge (branch|pull request|remote-tracking branch) ")
BULLET_RE = re.compile(r"^- .+$")
TRAILER_RE = re.compile(r"^[A-Za-z][A-Za-z-]*: ")
MAX_LINE = 80


def check_body(lines):
    """`lines` is everything after the subject and its blank separator.
    Each non-blank, non-trailer line must be a single-line `- ` bullet,
    80 chars max."""
    failures = []
    for i, line in enumerate(lines, start=1):
        if not line.strip() or TRAILER_RE.match(line):
            continue
        if not BULLET_RE.match(line):
            failures.append(f"body line {i}: not a '- bullet': {line!r}")
        elif len(line) > MAX_LINE:
            failures.append(f"body line {i}: {len(line)} chars (max {MAX_LINE}): {line!r}")
    return failures


def main(argv):
    if len(argv) != 1:
        print(__doc__)
        return 2
    with open(argv[0], encoding="utf-8") as f:
        text = f.read()

    lines = text.splitlines()
    subject = next((l for l in lines if l.strip()), "")

    if MERGE_RE.match(subject):
        return 0

    failures = []
    if not SUBJECT_RE.match(subject):
        failures.append(f"subject does not match 'type(scope)?: title': {subject!r}")

    subject_idx = lines.index(subject) if subject in lines else -1
    failures.extend(check_body(lines[subject_idx + 1 :]))

    if re.search(r"^Co-Authored-By:", text, re.IGNORECASE | re.MULTILINE):
        failures.append("no Co-Authored-By trailers (CLAUDE.md Git Conventions)")

    for f in failures:
        print(f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
