#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Enforce the Conventional Commits shape this repo actually uses:
`type(scope)?!?: description`, type/scope free-form (a subsystem name is as
valid as feat/fix/docs/...). No Co-Authored-By trailer. Merge commits are
exempt. Usage: check-commit-msg.py <msg-file>
"""
import re
import sys

SUBJECT_RE = re.compile(r"^[a-z][a-z0-9/+,-]*(\([a-z0-9_,. -]+\))?!?: .+$")
MERGE_RE = re.compile(r"^Merge (branch|pull request|remote-tracking branch) ")


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

    if re.search(r"^Co-Authored-By:", text, re.IGNORECASE | re.MULTILINE):
        failures.append("no Co-Authored-By trailers (CLAUDE.md Git Conventions)")

    for f in failures:
        print(f, file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
