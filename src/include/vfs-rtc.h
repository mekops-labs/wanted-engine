/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTC_NAME_MAX 15

/* `get` fills `sec` and `valid` (false after a power loss); a negative errno
 * means no answer. `set` writes the time and makes the device valid. Both
 * get the device's `ctx`. */
typedef struct rtc_ops_t {
    int (*get)(void *ctx, uint32_t *sec, bool *valid);
    int (*set)(void *ctx, uint32_t sec);
} rtc_ops_t;

typedef struct rtc_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most RTC_NAME_MAX */
    const rtc_ops_t *ops;
    void *ctx;
} rtc_device_desc_t;

/* Add a board device to the engine-wide table; the device named `main`
 * drives the system clock. Errors: -EINVAL bad description, -EEXIST name
 * taken, -ENOSPC table full. */
int RtcDeviceRegister(const rtc_device_desc_t *desc);

/* Set the system clock and the quality from a valid `main`. Returns
 * true when it did; false leaves everything alone. */
bool RtcBoot(void);

/* Drop every device. Only valid while no rtc driver instance exists. */
void RtcDevicesReset(void);
