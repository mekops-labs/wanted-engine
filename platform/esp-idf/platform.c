/* SPDX-License-Identifier: Apache-2.0 */

/* Platform surface not yet implemented on ESP-IDF: the platform VFS driver
 * table and the compiled-in (single-path) wapp loader used for the default
 * supervisor image. These come online with the storage layer / supervisor
 * embedding; they return errors until then so the engine links and boots.
 * The state-dir VFS driver (VfsPlatformFsInit) is real — see
 * vfs/vfs-esp-idf.c. Registry-driven load/unload (registry_flash.c) is real. */

#include <stddef.h>
#include <stdint.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>

/* The compiled-in default image path (src/wanted.c's supervisor bootstrap)
 * has no ESP-IDF backing yet — embedding it into the factory app partition is
 * a supervisor-bring-up concern, not the registry's. Registry-installed wapps
 * load via PlatformRegistryWappLoad (registry_flash.c) instead. */
int PlatformWappLoad(const char *name, wapp_t *wapp) {
    (void)name;
    (void)wapp;
    return 0;
}

static const vfs_driver_table_t esp_driver_table[] = {
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) { return esp_driver_table; }
