/* SPDX-License-Identifier: Apache-2.0 */

/* A host process has no reset to report: it was started, not restarted. */

#include <stddef.h>

#include <platform.h>

size_t PlatformResetReason(char *buf, size_t len) {
    if (buf != NULL && len > 0) {
        buf[0] = '\0';
    }
    return 0;
}
