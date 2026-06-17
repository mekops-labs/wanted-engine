/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX GPIO device driver, exposed to a wapp as a text level node.
 *
 * A wapp granted the `gpio` driver gets /dev/gpio in its namespace; the engine
 * opens the host GPIO character device and maps the node onto the NuttX GPIO
 * ioctl contract:
 *   write "1" -> drive the pin high; write "0" -> drive it low
 *   read       -> "1\n" or "0\n", the current pin level (one line, then EOF)
 *
 * The backing host node defaults to GPIO_DEVICE_PATH and is overridable through
 * the driver's launch-config options string, so one board can hand distinct
 * pins to distinct wapps.
 *
 * On the host-only scaffolding build the NuttX GPIO headers are absent, so the
 * level is held in memory; the real ioctl path compiles only for __NuttX__. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __NuttX__
#include <fcntl.h>
#include <nuttx/ioexpander/gpio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'G', 'p', 'i', 'o'};

#define GPIO_DEVICE_PATH "/dev/gpio0"
#define GPIO_PATH_MAX 32
#define GPIO_MAX_FDS 4 /* concurrent opens of the node */

struct gpio_fd_t {
    bool used;
    bool read_done; /* per-fd EOF latch for the level read */
#ifdef __NuttX__
    int host_fd;
#endif
};

struct vfs_driver_ctx_t {
    char path[GPIO_PATH_MAX];
    struct gpio_fd_t fds[GPIO_MAX_FDS];
#ifndef __NuttX__
    bool level; /* in-memory pin level for the scaffolding build */
#endif
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsGpioInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

    const char *path =
        (options != NULL && options[0] != '\0') ? options : GPIO_DEVICE_PATH;
    strncpy(ctx->path, path, sizeof(ctx->path) - 1);

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)path;
    (void)flags;

    int slot = -1;
    for (int i = 0; i < GPIO_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EMFILE;

#ifdef __NuttX__
    int host_fd = open(d->path, O_RDWR);
    if (host_fd < 0)
        return -errno;
    d->fds[slot].host_fd = host_fd;
#endif

    d->fds[slot].used = true;
    d->fds[slot].read_done = false;
    return slot;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
#ifdef __NuttX__
    close(d->fds[fd].host_fd);
#endif
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    (void)fd;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    s->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* read returns the current level as one line ("0\n"/"1\n"), then EOF. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (d->fds[fd].read_done)
        return 0;

    bool level;
#ifdef __NuttX__
    if (ioctl(d->fds[fd].host_fd, GPIOC_READ, (unsigned long)(uintptr_t)&level) <
        0)
        return -errno;
#else
    level = d->level;
#endif

    char line[2];
    line[0] = level ? '1' : '0';
    line[1] = '\n';

    size_t n = (nbyte < sizeof(line)) ? nbyte : sizeof(line);
    memcpy(buf, line, n);
    d->fds[fd].read_done = true;
    return (int)n;
}

/* write drives the pin: the first byte '1' sets it high, '0' sets it low. */
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (nbyte == 0)
        return 0;

    char c = ((const char *)buf)[0];
    if (c != '0' && c != '1')
        return -EINVAL;
    bool level = (c == '1');

#ifdef __NuttX__
    if (ioctl(d->fds[fd].host_fd, GPIOC_WRITE, (unsigned long)level) < 0)
        return -errno;
#else
    d->level = level;
#endif

    return (int)nbyte;
}
