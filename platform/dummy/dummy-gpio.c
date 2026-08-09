/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* In-memory GPIO fake for the unit tests, keyed by the grant's address field.
 * The address grammar is the ESP-IDF one, a decimal pin number, so a test
 * exercises the same grant string the hardware backing takes. */

#define DUMMY_GPIO_MAX_LINES 8

struct platform_gpio_t {
    bool used;
    char address[16];
    bool output;
    bool level;
};

static struct platform_gpio_t lines[DUMMY_GPIO_MAX_LINES];

static struct platform_gpio_t *find(const char *address) {
    for (int i = 0; i < DUMMY_GPIO_MAX_LINES; i++) {
        if (lines[i].used && strcmp(lines[i].address, address) == 0)
            return &lines[i];
    }
    return NULL;
}

void DummyGpioReset(void) { memset(lines, 0, sizeof(lines)); }

int DummyGpioSetLevel(const char *address, bool level) {
    struct platform_gpio_t *g = find(address);
    if (g == NULL)
        return -ENOENT;
    g->level = level;
    return 0;
}

int DummyGpioGetLevel(const char *address, bool *level) {
    const struct platform_gpio_t *g = find(address);
    if (g == NULL)
        return -ENOENT;
    *level = g->level;
    return 0;
}

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out) {
    if (cfg == NULL || cfg->address == NULL || out == NULL)
        return -EINVAL;
    if (strlen(cfg->address) >= sizeof(lines[0].address))
        return -EINVAL;
    /* Decimal only, matching the ESP-IDF address grammar. */
    for (const char *p = cfg->address; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -EINVAL;
    }
    if (find(cfg->address) != NULL)
        return -EBUSY;

    for (int i = 0; i < DUMMY_GPIO_MAX_LINES; i++) {
        if (lines[i].used)
            continue;
        memset(&lines[i], 0, sizeof(lines[i]));
        lines[i].used = true;
        lines[i].output = (cfg->direction == PLAT_GPIO_DIR_OUT);
        strncpy(lines[i].address, cfg->address, sizeof(lines[i].address) - 1);
        *out = &lines[i];
        return 0;
    }
    return -ENOSPC;
}

int PlatformGpioRead(const platform_gpio_t *g, bool *level) {
    if (g == NULL || !g->used || level == NULL)
        return -EINVAL;
    *level = g->level;
    return 0;
}

int PlatformGpioWrite(platform_gpio_t *g, bool level) {
    if (g == NULL || !g->used)
        return -EINVAL;
    if (!g->output)
        return -EPERM;
    g->level = level;
    return 0;
}

void PlatformGpioClose(platform_gpio_t *g) {
    if (g == NULL)
        return;
    memset(g, 0, sizeof(*g));
}
