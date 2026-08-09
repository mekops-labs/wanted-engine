/* SPDX-License-Identifier: Apache-2.0 */

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

#include <platform.h>

/* NuttX GPIO backing over the CONFIG_DEV_GPIO character devices; the grant's
 * address is a device path such as "/dev/gpio0". Pull and drive have no generic
 * GPIOC_* ioctl, so a grant asking for either is refused with -ENOTSUP. */

#define NUTTX_GPIO_MAX_LINES 8
#define NUTTX_GPIO_PATH_MAX 32

struct platform_gpio_t {
    bool used;
    bool output;
    char path[NUTTX_GPIO_PATH_MAX];
#ifdef __NuttX__
    int fd;
#else
    bool level;
#endif
};

static struct platform_gpio_t lines[NUTTX_GPIO_MAX_LINES];

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out) {
    if (cfg == NULL || cfg->address == NULL || out == NULL)
        return -EINVAL;
    if (cfg->address[0] != '/' || strlen(cfg->address) >= NUTTX_GPIO_PATH_MAX)
        return -EINVAL;
    if (cfg->pull != PLAT_GPIO_PULL_NONE || cfg->drive != PLAT_GPIO_DRIVE_PP)
        return -ENOTSUP;

    struct platform_gpio_t *slot = NULL;
    for (int i = 0; i < NUTTX_GPIO_MAX_LINES; i++) {
        if (lines[i].used && strcmp(lines[i].path, cfg->address) == 0)
            return -EBUSY;
        if (!lines[i].used && slot == NULL)
            slot = &lines[i];
    }
    if (slot == NULL)
        return -ENOSPC;

    memset(slot, 0, sizeof(*slot));
    strncpy(slot->path, cfg->address, sizeof(slot->path) - 1);
    slot->output = (cfg->direction == PLAT_GPIO_DIR_OUT);

#ifdef __NuttX__
    slot->fd = open(slot->path, O_RDWR);
    if (slot->fd < 0) {
        int rc = -errno;
        memset(slot, 0, sizeof(*slot));
        return rc;
    }
#endif

    slot->used = true;
    *out = slot;
    return 0;
}

int PlatformGpioRead(const platform_gpio_t *g, bool *level) {
    if (g == NULL || !g->used || level == NULL)
        return -EINVAL;
#ifdef __NuttX__
    bool v = false;
    if (ioctl(g->fd, GPIOC_READ, (unsigned long)(uintptr_t)&v) < 0)
        return -errno;
    *level = v;
#else
    *level = g->level;
#endif
    return 0;
}

int PlatformGpioWrite(platform_gpio_t *g, bool level) {
    if (g == NULL || !g->used)
        return -EINVAL;
    if (!g->output)
        return -EPERM;
#ifdef __NuttX__
    if (ioctl(g->fd, GPIOC_WRITE, (unsigned long)level) < 0)
        return -errno;
#else
    g->level = level;
#endif
    return 0;
}

void PlatformGpioClose(platform_gpio_t *g) {
    if (g == NULL || !g->used)
        return;
#ifdef __NuttX__
    close(g->fd);
#endif
    memset(g, 0, sizeof(*g));
}
