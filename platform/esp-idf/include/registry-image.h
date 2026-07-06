/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/* On-disk record for one registry entry's flash-partition placement. The
 * LittleFS index file "<name>@<version>.wapp" (registry.c) holds exactly this
 * struct — not the TAR bytes, which live in the "wapps" raw partition slot
 * this record names. registry_flash.c is the only writer/reader of the slot
 * field; registry.c reads `size` to answer PlatformRegistryRead. */
typedef struct {
    uint32_t magic;
    uint32_t slot;
    uint32_t size; /* actual stored image length, <= WAPP_IMAGE_SLOT_SIZE */
} wapp_image_meta_t;

#define WAPP_IMAGE_META_MAGIC 0x57415049u /* "WAPI" */

/* Registry metadata filename buffer: REGISTRY_ROOT + '/' + name + '@' +
 * version + REGISTRY_EXT + NUL, generously bounded above that fixed shape.
 * Deliberately not PATH_MAX (4096 on this newlib/xtensa toolchain) — these are
 * small, fixed-shape filenames, not arbitrary paths, and a PATH_MAX stack
 * buffer alone exceeds the main task's default stack (the "stack overflow in
 * task main" this replaced). */
#define WAPP_REG_PATH_MAX 96
