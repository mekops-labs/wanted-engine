/* SPDX-License-Identifier: Apache-2.0 */

/* A settable reset reason, so the suite can drive the paths a board reaches
 * only by crashing. Empty by default, which is a platform that cannot tell. */

#include <stddef.h>
#include <string.h>

#include "dummy-fs.h"
#include <platform.h>

static char reason[24];

void DummyResetReasonSet(const char *token) {
    if (token == NULL) {
        reason[0] = '\0';
        return;
    }
    strncpy(reason, token, sizeof(reason) - 1);
    reason[sizeof(reason) - 1] = '\0';
}

size_t PlatformResetReason(char *buf, size_t len) {
    if (buf == NULL || len == 0) {
        return 0;
    }
    size_t n = strlen(reason);
    if (n == 0 || n + 1 > len) {
        buf[0] = '\0';
        return 0;
    }
    memcpy(buf, reason, n + 1);
    return n;
}
