/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/* On-disk record for one registry entry's flash-partition placement. The
 * LittleFS index file holds exactly this struct; the TAR bytes live in the raw
 * partition slot it names. registry_flash.c is the only user of `slot`. */
typedef struct {
    uint32_t magic;
    uint32_t slot;
    uint32_t size; /* actual stored image length, <= WAPP_IMAGE_SLOT_SIZE */
} wapp_image_meta_t;

#define WAPP_IMAGE_META_MAGIC 0x57415049u /* "WAPI" */

/* Registry metadata filename buffer, bounded generously above the fixed shape
 * REGISTRY_ROOT/name@version.wapp. Deliberately not PATH_MAX: 4096 on this
 * toolchain, and one such stack buffer alone overflows the main task. */
#define WAPP_REG_PATH_MAX 96
