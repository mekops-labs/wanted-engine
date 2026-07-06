/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <platform-config.h>

/* The registry index (one small metadata file per entry: name/version/slot/
 * size) lives on the LittleFS "registry" partition, mounted at
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
 * partitions.csv) instead of the registry filesystem, so a running instance's
 * image can be exposed zero-copy via esp_partition_mmap. Fixed-size,
 * erase-sector-aligned (4096 B) slots make allocation a used/free bitmap scan
 * instead of a general allocator; a slot bounds the largest installable wapp
 * image. Slot count matches the "psram-s3" profile's MAX_WAPPS: registry.c's
 * mmap-handle table (registry_flash.c's g_mmapTable) is also sized off this
 * constant, so it bounds concurrently-*loaded* images too, not just installed
 * ones — a wapp `start` past this many concurrently-mapped images fails
 * -ENOMEM even with free internal RAM and PSRAM (found at the
 * PSRAM-allocator plan's M3, plans/wanted-engine-esp-idf-psram-allocator.md,
 * when MAX_WAPPS was raised without this). */
#define WAPP_IMAGE_PARTITION_LABEL "wapps"
#define WAPP_IMAGE_MAX_SLOTS 20
#define WAPP_IMAGE_SLOT_SIZE                                                   \
    (3200 * 1024 / WAPP_IMAGE_MAX_SLOTS) /* 160 KiB, 40 sectors */
