/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* On-disk record for one registry entry's flash-partition placement. The
 * LittleFS index file holds exactly this struct; the TAR bytes live in the raw
 * partition slot it names. registry_flash.c is the only user of `slot`. */
typedef struct {
    uint32_t magic;
    uint32_t slot;
    uint32_t size;     /* actual stored image length, <= slotSize */
    uint32_t slotSize; /* stride the slot offset was computed under */
} wapp_image_meta_t;

/* The index lives on a different partition from the bytes, so it outlives a
 * firmware whose slot geometry differs. A record naming a stride other than
 * this build's points at bytes that are no longer there, and the magic keeps
 * a record without the field from reading as one that has it. */
#define WAPP_IMAGE_META_MAGIC 0x57415032u /* "WAP2" */

/* True where `bytes` of record parse and name `slotSize`. Enumeration and
 * image reads share it, or the registry lists an entry no read can resolve. */
static inline bool WappImageMetaValid(const wapp_image_meta_t *meta,
                                      size_t bytes, uint32_t slotSize) {
    return bytes == sizeof(*meta) && meta->magic == WAPP_IMAGE_META_MAGIC &&
           meta->slotSize == slotSize;
}

/* Registry metadata filename buffer, bounded generously above the fixed shape
 * REGISTRY_ROOT/name@version.wapp. Deliberately not PATH_MAX: 4096 on this
 * toolchain, and one such stack buffer alone overflows the main task. */
#define WAPP_REG_PATH_MAX 96
