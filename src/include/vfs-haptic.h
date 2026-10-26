/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include <wanted-autoconf.h>

#define HAPTIC_NAME_MAX 15

/* The closed effect set; a device lists the ones it supports in a bit mask. */
#define HAPTIC_EFFECT_CLICK 0U
#define HAPTIC_EFFECT_DOUBLE 1U
#define HAPTIC_EFFECT_BUZZ 2U
#define HAPTIC_EFFECT_ALERT 3U
#define HAPTIC_EFFECT_COUNT 4U
#define HAPTIC_EFFECTS_ALL ((1U << HAPTIC_EFFECT_COUNT) - 1U)

/* Longest pulse a wapp can ask for. */
#define HAPTIC_PULSE_MS_MAX 2000U

/* Operations a board device supplies; every call gets the device's `ctx`.
 * `play` and `pulse` start the effect, replace a running one, and return at
 * once. `stop` ends any effect and is never an error. Errors are negative
 * errno values. */
typedef struct haptic_ops_t {
    int (*play)(void *ctx, unsigned effect);
    int (*pulse)(void *ctx, unsigned ms);
    void (*stop)(void *ctx);
    bool (*busy)(const void *ctx);
} haptic_ops_t;

typedef struct haptic_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most HAPTIC_NAME_MAX */
    unsigned effects; /* bit mask of HAPTIC_EFFECT_* the device plays */
    const haptic_ops_t *ops;
    void *ctx;
} haptic_device_desc_t;

/* Add a board device to the engine-wide table; a grant selects it by name.
 * Errors: -EINVAL bad description, -EEXIST name taken, -ENOSPC table full. */
int HapticDeviceRegister(const haptic_device_desc_t *desc);

/* Drop every device and close every pin. Only valid while no haptic driver
 * instance exists. */
void HapticDevicesReset(void);
