/* SPDX-License-Identifier: Apache-2.0 */

/* RTC slow memory survives a watchdog reset, a panic and a commanded reboot,
 * and startup never clears a NOLOAD section. The region is 8 KiB on this part,
 * shared with anything else that must outlive a reset. */

#include <stddef.h>

#include <esp_attr.h>
#include <platform.h>
#include <wanted-autoconf.h>

#if CONFIG_WANTED_LOG_PERSIST_CAP > 0

/* Two buffers plus the store's own header; the header is small and the cap is
 * what the configuration bounds. */
#define PERSIST_MEM_BYTES (CONFIG_WANTED_LOG_PERSIST_CAP * 2 + 64)

static RTC_NOINIT_ATTR unsigned char persistMem[PERSIST_MEM_BYTES];

void *PlatformPersistMem(size_t *len) {
    if (len != NULL) {
        *len = sizeof(persistMem);
    }
    return persistMem;
}

#else

void *PlatformPersistMem(size_t *len) {
    if (len != NULL) {
        *len = 0;
    }
    return NULL;
}

#endif
