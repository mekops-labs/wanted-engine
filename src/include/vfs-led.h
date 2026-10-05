/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include <wanted-autoconf.h>

#define LED_NAME_MAX 15

/* Operations a board device supplies; every call gets the device's `ctx`.
 * `fade` ramps to `level` over `ms` and returns at once. `get` returns the
 * level the LED shows now. Errors are negative errno values. */
typedef struct led_ops_t {
    int (*set)(void *ctx, unsigned level);
    int (*fade)(void *ctx, unsigned level, unsigned ms);
    unsigned (*get)(const void *ctx);
} led_ops_t;

typedef struct led_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most LED_NAME_MAX */
    unsigned max;     /* max_brightness, at least 1 */
    const led_ops_t *ops;
    void *ctx;
    bool hwFade; /* fades run without further calls */
} led_device_desc_t;

/* Add a board device to the engine-wide table; a grant selects it by name.
 * Errors: -EINVAL bad description, -EEXIST name taken, -ENOSPC table full. */
int LedDeviceRegister(const led_device_desc_t *desc);

/* Run the idle and heartbeat triggers. The engine loop calls it about once a
 * second. */
void LedTick(void);

/* Drop every device and close every pin. Only valid while no led driver
 * instance exists. */
void LedDevicesReset(void);

#ifdef CONFIG_WANTED_VFS_INPUT
#include <vfs-input.h>

/* The input core calls this for each event pushed to `dev`. */
void LedActivity(const input_device_t *dev);
#endif
