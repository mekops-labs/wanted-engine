/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF writable volume root — the LittleFS mount point the registry and wapp
 * state directories live under. Mounting the partition is done at startup. */

#include <platform.h>

#define VOLUME_ROOT "/data"

const char *PlatformVolumeRoot(void) { return VOLUME_ROOT; }
