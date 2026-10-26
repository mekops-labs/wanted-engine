/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <wanted-autoconf.h>

#define POWER_NAME_MAX 15
#define POWER_WORD_MAX 15
#define POWER_WAKE_MAX 4

/* The five fixed modes, from most to least awake. */
typedef enum power_mode_t {
    POWER_ACTIVE = 0,
    POWER_STANDBY,
    POWER_SUSPEND,
    POWER_SLEEP,
    POWER_OFF,
    POWER_MODE_COUNT
} power_mode_t;

#define POWER_BIT(m) (1U << (m))

/* A wake source a board can arm: a word for `policy` and `wake`, and the
 * modes it can be armed in. */
typedef struct power_wake_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most POWER_WORD_MAX */
    unsigned modes;   /* POWER_BIT() of POWER_STANDBY, SUSPEND, SLEEP */
} power_wake_desc_t;

/* What the engine hands a board hook when it enters a sleeping mode. */
typedef struct power_arm_t {
    unsigned wakeMask; /* bit i arms the board's wake source i */
    uint64_t timerUs;  /* wake after this long, or 0 for no timer */
    bool powerKeyOnly; /* off that could not cut power: only the key wakes */
} power_arm_t;

/* Why the CPU came back. */
typedef enum power_cause_t {
    POWER_CAUSE_COLD = 0,
    POWER_CAUSE_TIMER,
    POWER_CAUSE_EXTERNAL
} power_cause_t;

/* Operations a board supplies, each given the device's `ctx`. A mode the board
 * does not list needs no hook. Errors are negative errno values. */
typedef struct power_ops_t {
    /* Switches the rails and arms the wake sources. */
    int (*suspend)(void *ctx, const power_arm_t *arm);
    /* Restores them; returns the cause's wake word, or NULL for the default. */
    const char *(*resume)(void *ctx, power_cause_t cause);
    /* Prepares a deep sleep; 0 lets the engine sleep. */
    int (*sleep)(void *ctx, const power_arm_t *arm);
    /* Cuts power, and returns only when it could not. */
    int (*poweroff)(void *ctx);
    bool (*externalPower)(void *ctx);
    size_t (*rails)(void *ctx, char *buf, size_t len);
} power_ops_t;

typedef struct power_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most POWER_NAME_MAX */
    unsigned modes;   /* POWER_BIT() of STANDBY, SUSPEND, SLEEP; static */
    const power_wake_desc_t *wakes; /* static array, or NULL */
    unsigned wakeCount;             /* at most POWER_WAKE_MAX */
    const power_ops_t *ops;
    void *ctx;
    bool cutsWithExternalPower; /* poweroff cuts power with VBUS present */
} power_device_desc_t;

/* Add a board device to the engine-wide table. After a deep sleep it also
 * restores the ladder the engine saved. Errors: -EINVAL bad description,
 * -EEXIST name taken, -ENOSPC table full. */
int PowerDeviceRegister(const power_device_desc_t *desc);

/* Notices a board's sampler raises; the engine applies the grace and the
 * transition. */
typedef enum power_notice_t {
    POWER_NOTICE_KEY_HELD = 0,
    POWER_NOTICE_LOW_BATTERY
} power_notice_t;

/* Returns -ENOENT when no device has the name. */
int PowerNotify(const char *name, power_notice_t notice);

/* A driver or a board takes part in suspend and resume. Suspend callbacks run
 * in reverse registration order, then the board's hook; resume runs the other
 * way. A failing suspend callback does not stop the transition. */
typedef struct power_client_t {
    const char *name;
    int (*suspend)(void *ctx);
    int (*resume)(void *ctx);
    void *ctx;
} power_client_t;

/* Errors: -EINVAL both callbacks missing, -ENOSPC table full. */
int PowerClientRegister(const power_client_t *client);

/* Run the ladder timers and the pending transition. The engine loop calls it
 * about every 100 ms. */
void PowerTick(void);

/* Drop every device and client. Only valid while no power driver instance
 * exists. */
void PowerDevicesReset(void);
