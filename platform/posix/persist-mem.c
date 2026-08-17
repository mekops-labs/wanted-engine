/* SPDX-License-Identifier: Apache-2.0 */

/* A host keeps no memory across a reset: its process starts with a fresh
 * address space, so the previous run's log is gone with it. */

#include <stddef.h>

#include <platform.h>

void *PlatformPersistMem(size_t *len) {
    if (len != NULL) {
        *len = 0;
    }
    return NULL;
}
