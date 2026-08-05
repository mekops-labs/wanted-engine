/* SPDX-License-Identifier: Apache-2.0 */

/* Platform surface: the platform VFS driver table and the single-path wapp
 * loader for the compiled-in supervisor image. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>

/* ESP-IDF has no host filesystem to stage the compiled-in supervisor image on,
 * so EMBED_FILES links it into the app binary as a linker section, read
 * zero-copy from the app partition's own mapped flash. */
extern const uint8_t _binary_supervisor_tar_start[];
extern const uint8_t _binary_supervisor_tar_end[];

/* `name` is always SUPERVISOR_IMAGE_PATH, since a per-wapp launch config
 * resolves through PlatformRegistryWappLoad instead. There is exactly one
 * embedded image, so the argument goes unused. */
int PlatformWappLoad(const char *name, wapp_t *wapp) {
    (void)name;
    if (wapp == NULL)
        return -EINVAL;

    wapp->layers[0] = (uint8_t *)_binary_supervisor_tar_start;
    /* start/end are the linker's boundary symbols for one embedded blob, not
     * two unrelated objects — the standard EMBED_FILES size idiom. */
    const uint8_t *start = _binary_supervisor_tar_start;
    const uint8_t *end = _binary_supervisor_tar_end;
    /* cppcheck-suppress subtractPointers */
    wapp->layer_lens[0] = (size_t)(end - start);
    wapp->layer_cnt = 1;
    return 0;
}

static const vfs_driver_table_t esp_driver_table[] = {
    {"wifi", VfsWifiInit},
    {NULL, NULL},
};

const vfs_driver_table_t *PlatformDriverTable(void) { return esp_driver_table; }
