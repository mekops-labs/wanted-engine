/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"

#include <platform.h>

/* ESP-IDF GPIO backing over esp_driver_gpio.
 *
 * The grant's address field is a decimal GPIO number. Direction, pull, and
 * drive come from the grant and are applied once, at open. An output is
 * configured as GPIO_MODE_INPUT_OUTPUT so a read of `value` returns the level
 * on the pad rather than the last value written — an open-drain output held
 * low by an external device reads 0, which is the useful answer. */

#define ESP_GPIO_MAX_LINES 8

struct platform_gpio_t {
    bool used;
    gpio_num_t pin;
    bool output;
};

static struct platform_gpio_t lines[ESP_GPIO_MAX_LINES];

/* Decimal GPIO number, or -1 when the address is not one. */
static int parsePin(const char *address) {
    if (address == NULL || address[0] == '\0')
        return -1;
    int n = 0;
    for (const char *p = address; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out) {
    if (cfg == NULL || out == NULL)
        return -EINVAL;

    int pin = parsePin(cfg->address);
    if (pin < 0 || !GPIO_IS_VALID_GPIO((gpio_num_t)pin))
        return -EINVAL;
    if (cfg->direction == PLAT_GPIO_DIR_OUT &&
        !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin))
        return -EINVAL;

    struct platform_gpio_t *slot = NULL;
    for (int i = 0; i < ESP_GPIO_MAX_LINES; i++) {
        if (lines[i].used && lines[i].pin == (gpio_num_t)pin)
            return -EBUSY;
        if (!lines[i].used && slot == NULL)
            slot = &lines[i];
    }
    if (slot == NULL)
        return -ENOSPC;

    gpio_config_t io;
    memset(&io, 0, sizeof(io));
    io.pin_bit_mask = 1ULL << (unsigned)pin;
    io.intr_type = GPIO_INTR_DISABLE;
    io.pull_up_en = (cfg->pull == PLAT_GPIO_PULL_UP) ? GPIO_PULLUP_ENABLE
                                                     : GPIO_PULLUP_DISABLE;
    io.pull_down_en = (cfg->pull == PLAT_GPIO_PULL_DOWN)
                          ? GPIO_PULLDOWN_ENABLE
                          : GPIO_PULLDOWN_DISABLE;

    if (cfg->direction == PLAT_GPIO_DIR_OUT) {
        io.mode = (cfg->drive == PLAT_GPIO_DRIVE_OD) ? GPIO_MODE_INPUT_OUTPUT_OD
                                                     : GPIO_MODE_INPUT_OUTPUT;
    } else {
        if (cfg->drive == PLAT_GPIO_DRIVE_OD)
            return -ENOTSUP; /* drive mode is meaningless on an input */
        io.mode = GPIO_MODE_INPUT;
    }

    gpio_reset_pin((gpio_num_t)pin);
    if (gpio_config(&io) != ESP_OK)
        return -EIO;
    if (cfg->direction == PLAT_GPIO_DIR_OUT)
        gpio_set_level((gpio_num_t)pin, 0);

    slot->used = true;
    slot->pin = (gpio_num_t)pin;
    slot->output = (cfg->direction == PLAT_GPIO_DIR_OUT);
    *out = slot;
    return 0;
}

int PlatformGpioRead(const platform_gpio_t *g, bool *level) {
    if (g == NULL || !g->used || level == NULL)
        return -EINVAL;
    *level = gpio_get_level(g->pin) != 0;
    return 0;
}

int PlatformGpioWrite(platform_gpio_t *g, bool level) {
    if (g == NULL || !g->used)
        return -EINVAL;
    if (!g->output)
        return -EPERM;
    return (gpio_set_level(g->pin, level ? 1 : 0) == ESP_OK) ? 0 : -EIO;
}

void PlatformGpioClose(platform_gpio_t *g) {
    if (g == NULL || !g->used)
        return;
    gpio_reset_pin(g->pin);
    memset(g, 0, sizeof(*g));
}
