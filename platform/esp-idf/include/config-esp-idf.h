/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <platform-config.h>
#include <wanted-autoconf.h>

/* The registry index — one small metadata file per entry — lives on the
 * LittleFS "persist" partition, mounted at PlatformVolumeRoot(). The relative
 * default only makes sense against a process CWD, which a board has none of. */
#undef REGISTRY_ROOT
#define REGISTRY_ROOT "/data/registry"

/* Upper bound on registry entries materialised when resolving a wapp whose
 * version is unspecified. Embedded targets avoid VLAs, so this caps the
 * on-stack scan buffer (mirrors the NuttX platform's cap). */
#define REGISTRY_MAX_ENTRIES 50

/* Wapp image bytes live in a dedicated raw flash partition, so a running
 * instance's image is exposed zero-copy through esp_partition_mmap. Slots are
 * fixed-size and erase-sector-aligned, and one bounds the largest image. */
#define WAPP_IMAGE_PARTITION_LABEL "wapps"
#define WAPP_IMAGE_MAX_SLOTS CONFIG_WANTED_MAX_WAPPS
#define WAPP_IMAGE_SLOT_SIZE (CONFIG_WANTED_MAX_WAPP_IMAGE_KB * 1024)
