/* SPDX-License-Identifier: Apache-2.0 */

/* Shared wapp image load/unload: expose a wapp image file as the single TarFS
 * layer. These touch no platform lifecycle state, so they are common to every
 * POSIX target; the thread/stop lifecycle stays per-platform.
 *
 * Linux maps the file (zero-copy). NuttX copies it into a heap buffer: its mmap
 * is a full RAM copy anyway, and the heap may extend into PSRAM. On ESP32 the
 * image lives in SPI flash (LittleFS) and the destination may be PSRAM-backed;
 * a single large read straight into a PSRAM buffer corrupts (the SPI-flash MTD
 * read target cannot be PSRAM), so the bytes are read through a small
 * internal-RAM bounce buffer and copied into the image buffer with the CPU. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifdef __NuttX__
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <platform.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

#include <debug_trace.h>

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

#ifdef __NuttX__

/* Bounce buffer for image reads. Static so it lives in .bss — internal RAM,
 * never PSRAM: the SPI-flash read target stays in internal RAM and the bytes
 * are copied into the (possibly PSRAM) image buffer with the CPU. The registry
 * cache (platform/nuttx/api/registry.c) is what guarantees these reads run only
 * while no wapp is active — an ESP32 SPI-flash read returns corrupt data while
 * another task holds live PSRAM. A mutex serialises the shared buffer. */
#define IMAGE_READ_CHUNK 2048
static uint8_t g_image_bounce[IMAGE_READ_CHUNK];
static pthread_mutex_t g_image_bounce_lock = PTHREAD_MUTEX_INITIALIZER;

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    struct stat st;
    uint8_t *img;
    int fd;
    size_t off;

    if (NULL == wapp) {
        return -EINVAL;
    }

    DEBUG_TRACE("Opening: %s\n", path);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        FATAL(-errno, "can't open wapp: %s", path);
    }

    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        int e = errno;
        close(fd);
        FATAL(-EINVAL, "bad wapp size: %s errno=%d", path, e);
    }

    img = (uint8_t *)PlatformExtramMalloc((size_t)st.st_size);
    if (NULL == img) {
        close(fd);
        FATAL(-ENOMEM, "can't alloc image (size=%lld)", (long long)st.st_size);
    }

    pthread_mutex_lock(&g_image_bounce_lock);
    for (off = 0; off < (size_t)st.st_size;) {
        size_t want = (size_t)st.st_size - off;
        ssize_t got;
        if (want > sizeof(g_image_bounce))
            want = sizeof(g_image_bounce);
        errno = 0;
        got = read(fd, g_image_bounce, want);
        if (got <= 0) {
            /* NuttX may return the negative errno directly (errno left 0); a
             * zero return is an unexpected mid-file EOF. */
            int e = (got < 0) ? (errno ? -errno : (int)got) : -EIO;
            pthread_mutex_unlock(&g_image_bounce_lock);
            PlatformExtramFree(img);
            close(fd);
            FATAL(e, "read failed (%d) at %zu of %lld: %s", e, off,
                  (long long)st.st_size, path);
        }
        memcpy(img + off, g_image_bounce, (size_t)got);
        off += (size_t)got;
    }
    pthread_mutex_unlock(&g_image_bounce_lock);

    close(fd);

    wapp->layers[0] = img;
    wapp->layer_lens[0] = (size_t)st.st_size;
    wapp->layer_cnt = 1;
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (NULL == wapp) {
        return -EINVAL;
    }

    PlatformExtramFree(wapp->layers[0]);
    return 0;
}

#else /* !__NuttX__ */

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
    if (img == MAP_FAILED) {
        int e = errno;
        fclose(f);
        FATAL(-e, "can't map file %s (size=%ld) errno=%d", path, filesize, e);
    }

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

#endif /* __NuttX__ */
