#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Registry version for the flasher image, derived from the tree it is built
# from. A registry version field follows a bounded image-tag grammar, which a
# `git describe` string does not, and a tag's leading `v` is dropped.
#
# Usage: registry-version.sh <repo>
#   0.3.3          built at a tag
#   0.3.3-abc123   built past a tag: the tag plus the 6-character commit

set -euo pipefail
cd "${1:?usage: registry-version.sh <repo>}"

if tag=$(git describe --tags --exact-match 2>/dev/null); then
    printf '%s\n' "${tag#v}"
    exit 0
fi

# No tag is a shallow clone or a fresh history, not a failure: the commit alone
# identifies the source.
if tag=$(git describe --tags --abbrev=0 2>/dev/null); then
    printf '%s-%s\n' "${tag#v}" "$(git rev-parse --short=6 HEAD)"
else
    printf '0.0.0-%s\n' "$(git rev-parse --short=6 HEAD)"
fi
