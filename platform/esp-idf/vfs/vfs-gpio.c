/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF GPIO device driver: /dev/gpio drives one pin over esp_driver_gpio,
 * same text contract as the NuttX driver — write "1"/"0" sets the level, read
 * yields "1\n"/"0\n" then EOF. The pin is a decimal in the options string,
 * default GPIO_DEFAULT_PIN. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'G', 'p', 'i', 'o'};

#define GPIO_DEFAULT_PIN 21 /* Waveshare ESP32 One onboard LED */
#define GPIO_MAX_FDS 4

struct gpio_fd_t {
    bool used;
    bool read_done; /* per-fd EOF latch */
};

struct vfs_driver_ctx_t {
    gpio_num_t pin;
    struct gpio_fd_t fds[GPIO_MAX_FDS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

/* Decimal pin from the options string; -1 on empty/malformed (caller defaults).
 */
static int parsePin(const char *options) {
    if (options == NULL || options[0] == '\0')
        return -1;
    int n = 0;
    for (const char *p = options; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

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

    int pin = parsePin(options);
    ctx->pin = (gpio_num_t)(pin >= 0 ? pin : GPIO_DEFAULT_PIN);
    gpio_reset_pin(ctx->pin);
    gpio_set_direction(ctx->pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(ctx->pin, 0);

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

    d->fds[slot].used = true;
    d->fds[slot].read_done = false;
    return slot;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
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

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (d->fds[fd].read_done)
        return 0;

    char line[2];
    line[0] = gpio_get_level(d->pin) ? '1' : '0';
    line[1] = '\n';

    size_t n = (nbyte < sizeof(line)) ? nbyte : sizeof(line);
    memcpy(buf, line, n);
    d->fds[fd].read_done = true;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (nbyte == 0)
        return 0;

    char c = ((const char *)buf)[0];
    if (c != '0' && c != '1')
        return -EINVAL;
    gpio_set_level(d->pin, c == '1' ? 1 : 0);

    return (int)nbyte;
}
