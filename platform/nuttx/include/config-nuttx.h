/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <platform-config.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

/* Where a board carves the flash into two filesystems, images live on the one
 * sized for them, so wapp state cannot exhaust the space the registry needs.
 * The relative default only makes sense against a process CWD, which a board
 * has none of. */
#ifdef CONFIG_RP23XX_FLASH_MTD_REGISTRY_MOUNTPOINT
#undef REGISTRY_ROOT
#define REGISTRY_ROOT CONFIG_RP23XX_FLASH_MTD_REGISTRY_MOUNTPOINT
#endif

/* Upper bound on registry entries materialised when resolving a wapp whose
 * version is unspecified. Embedded targets avoid VLAs, so this caps the
 * on-stack scan buffer. */
#define REGISTRY_MAX_ENTRIES 50
