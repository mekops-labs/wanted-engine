/* SPDX-License-Identifier: Apache-2.0 */

/* Platform surface not yet implemented on ESP-IDF: the platform VFS driver
 * table and registry image write / wapp load. These come online with the
 * storage layer; they return errors until then so the engine links and boots.
 * The state-dir VFS driver (VfsPlatformFsInit) is real — see
 * vfs/vfs-esp-idf.c. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>

/* Wapp image load/unload map a registry image into the read-only flash window
 * (esp_partition_mmap) with the storage layer; until then loading is a no-op so
 * the wapp lifecycle links. */
int PlatformWappLoad(const char *name, wapp_t *wapp) {
    (void)name;
    (void)wapp;
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    (void)wapp;
    return 0;
}

static const vfs_driver_table_t esp_driver_table[] = {
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) { return esp_driver_table; }

int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    (void)s;
    (void)ref;
    (void)buf;
    (void)nbytes;
    return -ENOSYS;
}

int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    (void)entry;
    (void)w;
    return -ENOSYS;
}
