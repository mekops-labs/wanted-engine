#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Registry version for the flasher image, derived from the tree it is built
# from. A registry version field is bounded and follows the image-tag grammar
# ([A-Za-z0-9_] then [A-Za-z0-9._-]); a `git describe` string satisfies
# neither. The version is reduced to what identifies the source:
#
#   0.3.3          built at a tag
#   0.3.3-abc123   built past a tag: the tag plus the 6-character commit
#
# A tag's leading `v` is dropped, so the version is bare semver. A dirty tree
# carries no marker: the character is needed for double-digit version
# components, and a build worth seeding is a committed one.
#
# Usage: registry-version.sh <repo>

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
