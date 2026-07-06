/* SPDX-License-Identifier: Apache-2.0 */

/* Platform surface: the platform VFS driver table (wifi is real — see
 * vfs/vfs-wifi.c; gpio is a future milestone) and the compiled-in
 * (single-path) wapp loader for the default supervisor image. The state-dir
 * VFS driver (VfsPlatformFsInit) is real — see vfs/vfs-esp-idf.c.
 * Registry-driven load/unload (registry_flash.c) is real. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>

/* ESP-IDF has no host filesystem to stage the compiled-in supervisor image on
 * (the NuttX port's ROMFS/SD-card equivalent), so `main`'s EMBED_FILES links
 * it straight into the app binary as a linker section — a zero-copy read,
 * same as the registry's esp_partition_mmap XIP path, just backed by the app
 * partition's own mapped flash instead of a raw data partition.
 * `_binary_supervisor_tar_{start,end}` are ESP-IDF's standard EMBED_FILES
 * symbol names, generated from the embedded file's basename
 * ("supervisor.tar" — see platform/esp-idf/project/main/CMakeLists.txt). */
extern const uint8_t _binary_supervisor_tar_start[];
extern const uint8_t _binary_supervisor_tar_end[];

/* `name` is always SUPERVISOR_IMAGE_PATH (src/wanted.c's supervisor
 * bootstrap; a per-wapp launch config never resolves through this path — see
 * PlatformRegistryWappLoad) — there is exactly one embedded image, so it is
 * unused rather than compared against. */
int PlatformWappLoad(const char *name, wapp_t *wapp) {
    (void)name;
    if (wapp == NULL)
        return -EINVAL;

    wapp->layers[0] = (uint8_t *)_binary_supervisor_tar_start;
    wapp->layer_lens[0] =
        (size_t)(_binary_supervisor_tar_end - _binary_supervisor_tar_start);
    wapp->layer_cnt = 1;
    return 0;
}

static const vfs_driver_table_t esp_driver_table[] = {
    {"wifi", VfsWifiInit},
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) { return esp_driver_table; }
