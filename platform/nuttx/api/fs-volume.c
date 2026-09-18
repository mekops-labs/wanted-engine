/* SPDX-License-Identifier: Apache-2.0 */

#include <config-nuttx.h>
#include <platform.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

/* A board bringing up its internal-flash MTD names its own mountpoint here;
 * the sim has no such board bring-up and keeps the generic relative
 * default. */
#ifdef CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT
const char *PlatformVolumeRoot(void) {
    return CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT;
}
#else
const char *PlatformVolumeRoot(void) { return VOLUME_ROOT; }
#endif
