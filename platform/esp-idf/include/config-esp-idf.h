/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <platform-config.h>
#include <wanted-autoconf.h>

/* The registry records live on the LittleFS "persist" partition, mounted at
 * PlatformVolumeRoot(). The relative default only makes sense against a process
 * CWD, which a board has none of. */
#undef REGISTRY_ROOT
#define REGISTRY_ROOT "/data/registry"

/* Wapp image bytes live in a dedicated raw flash partition and are mapped from
 * it, so a running instance's image costs no RAM. */
#define WAPP_IMAGE_PARTITION_LABEL "wapps"
