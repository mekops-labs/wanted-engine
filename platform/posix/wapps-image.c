/* SPDX-License-Identifier: Apache-2.0 */

/* Wapp image load and unload for the shared POSIX platforms. */

#include <errno.h>
#include <stdint.h>

#include <platform.h>
#include <registry.h>
#include <wanted-api.h>

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    uint8_t *img;
    size_t len;
    int rc;

    if (NULL == wapp) {
        return -EINVAL;
    }
    rc = PlatformFileMap(path, &img, &len);
    if (rc < 0)
        return rc;

    wapp->layers[0] = img;
    wapp->layer_lens[0] = len;
    wapp->layer_cnt = 1;
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (NULL == wapp) {
        return -EINVAL;
    }
    if (RegistryUnload(wapp))
        return 0;

    return PlatformFileUnmap(wapp->layers[0], wapp->layer_lens[0]);
}
