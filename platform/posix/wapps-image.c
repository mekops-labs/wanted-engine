/* SPDX-License-Identifier: Apache-2.0 */

/* Shared wapp image load/unload: mmap a wapp image file as the single TarFS
 * layer. These touch no platform lifecycle state, so they are common to every
 * POSIX target; the thread/stop lifecycle stays per-platform. */

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

#include <platform.h>
#include <wanted-api.h>

#include <debug_trace.h>

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;

    if (NULL == wapp) {
        return -EINVAL;
    }

    DEBUG_TRACE("Opening: %s\n", path);

    f = fopen(path, "rb");

    if (NULL == f) {
        FATAL(-errno, "can't open wapp: %s", path);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED)
        FATAL(-errno, "can't map file");

    wapp->layers[0] = img;
    wapp->layer_lens[0] = filesize;
    wapp->layer_cnt = 1;

    fclose(f);
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (NULL == wapp) {
        return -EINVAL;
    }

    if (munmap(wapp->layers[0], wapp->layer_lens[0]) < 0)
        FATAL(-errno, "can't unmap file");
    return 0;
}
