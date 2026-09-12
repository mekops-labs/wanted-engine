/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <registry-meta.h>

/* On-disk record for one registry entry: the metadata every backend stores,
 * followed by this backing's flash-partition placement. The LittleFS index
 * file holds exactly this struct; the TAR bytes live in the slot it names. */
typedef struct {
    registry_meta_t meta;
    uint32_t slot;
    uint32_t slotSize; /* stride the slot offset was computed under */
} wapp_image_meta_t;

/* True where `bytes` of record parse and name `slotSize` — a mismatch means
 * a record from a firmware whose slot geometry differs, naming bytes that
 * moved. Shared by enumeration and image reads. */
static inline bool WappImageMetaValid(const wapp_image_meta_t *meta,
                                      size_t bytes, uint32_t slotSize) {
    return bytes == sizeof(*meta) && RegistryMetaValid(&meta->meta, bytes) &&
           meta->slotSize == slotSize;
}

/* Registry metadata filename buffer, bounded generously above the fixed shape
 * REGISTRY_ROOT/name@version.wapp. Deliberately not PATH_MAX: 4096 on this
 * toolchain, and one such stack buffer alone overflows the main task. */
#define WAPP_REG_PATH_MAX 96
