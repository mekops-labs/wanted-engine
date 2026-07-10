/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <platform-config.h>
#include <wanted-config.h>

/* The registry index (one small metadata file per entry: name/version/slot/
 * size) lives on the LittleFS "persist" partition, mounted at
 * PlatformVolumeRoot() ("/data", platform/esp-idf/fs-volume.c). Overrides
 * platform-config.h's relative default, which only makes sense against a
 * process CWD. */
#undef REGISTRY_ROOT
#define REGISTRY_ROOT "/data/registry"

/* Upper bound on registry entries materialised when resolving a wapp whose
 * version is unspecified. Embedded targets avoid VLAs, so this caps the
 * on-stack scan buffer (mirrors the NuttX platform's cap). */
#define REGISTRY_MAX_ENTRIES 50

/* Wapp image bytes live in a dedicated raw flash partition ("wapps",
 * generated into partitions.csv by an OTA_PROFILE — see partitions.csv.in)
 * instead of the registry filesystem, so a running instance's image can be
 * exposed zero-copy via esp_partition_mmap. Fixed-size, erase-sector-aligned
 * (4096 B) slots make allocation a used/free bitmap scan instead of a
 * general allocator; a slot bounds the largest installable wapp image.
 *
 * WAPP_IMAGE_MAX_SLOTS is an alias for MAX_WAPPS, not an independently
 * hand-set constant: registry_flash.c's mmap-handle table (g_mmapTable) is
 * sized off this same constant, so it bounds concurrently-*loaded* images
 * too, not just installed ones -- a wapp `start` past this many
 * concurrently-mapped images fails -ENOMEM even with free internal RAM and
 * PSRAM. Deriving both this and the "wapps" partition size from the one
 * MAX_WAPPS knob avoids hand-editing three places in sync. Slot size is
 * fixed across every profile -- only the slot count (MAX_WAPPS) varies. */
#define WAPP_IMAGE_PARTITION_LABEL "wapps"
#define WAPP_IMAGE_MAX_SLOTS MAX_WAPPS
#define WAPP_IMAGE_SLOT_SIZE (160 * 1024) /* 40 sectors */
