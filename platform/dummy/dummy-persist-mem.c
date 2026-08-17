/* SPDX-License-Identifier: Apache-2.0 */

/* A static buffer standing in for memory a reset does not clear, so the host
 * suite can exercise the store's rotation. DummyPersistMemClear() is the
 * equivalent of losing power. */

#include <stddef.h>
#include <string.h>

#include "dummy-fs.h"
#include <platform.h>
#include <wanted-autoconf.h>

#define PERSIST_MEM_BYTES (CONFIG_WANTED_LOG_PERSIST_CAP * 2 + 64)

static unsigned char persistMem[PERSIST_MEM_BYTES];

void *PlatformPersistMem(size_t *len) {
    if (len != NULL) {
        *len = sizeof(persistMem);
    }
    return persistMem;
}

void DummyPersistMemClear(void) { memset(persistMem, 0, sizeof(persistMem)); }
