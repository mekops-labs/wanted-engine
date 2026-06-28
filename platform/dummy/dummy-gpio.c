/* SPDX-License-Identifier: Apache-2.0 */

/* In-memory GPIO level node for the unit-test platform. Holds the pin level in
 * memory so the `gpio` driver resolves and its write "1"/"0" / read contract is
 * exercisable without hardware. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'G', 'p', 'i', 'o'};

#define GPIO_MAX_FDS 4

struct gpio_fd_t {
    bool used;
    bool read_done;
};

struct vfs_driver_ctx_t {
    struct gpio_fd_t fds[GPIO_MAX_FDS];
    bool level;
};

static int _Destroy(struct vfs_driver_t *d) {
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)path;
    (void)flags;
    for (int i = 0; i < GPIO_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            d->fds[i].used = true;
            d->fds[i].read_done = false;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    (void)d;
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
    line[0] = d->level ? '1' : '0';
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
    d->level = (c == '1');

    return (int)nbyte;
}

vfs_driver_t *VfsGpioInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;
    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver)
        return NULL;

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == ctx) {
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

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
