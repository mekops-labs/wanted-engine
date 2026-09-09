/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

/* Ordering for image-tag versions: `<major>.<minor>.<patch>` with an optional
 * leading `v` and an optional `-<commit>` suffix. See docs/design.md. */

/* True when `a` names a strictly newer build than `b`. Two builds past the
 * same tag are unordered and report false, so a caller keeps what it has. */
bool VersionNewer(const char *a, const char *b);
