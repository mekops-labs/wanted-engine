/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <rtc-time.h>

/* A register bus, one transaction per call. `read` writes the register
 * address, then reads `len` bytes from consecutive registers; `write` sends
 * the address and `len` bytes. Both return 0 or a negative errno. */
typedef struct rtc_bus_t {
    int (*read)(void *ctx, uint8_t reg, uint8_t *buf, size_t len);
    int (*write)(void *ctx, uint8_t reg, const uint8_t *buf, size_t len);
    void *ctx;
} rtc_bus_t;

/* A clock chip over a bus. `get` fills `tm` and `valid` (false when the chip
 * cannot vouch for the time). `set` writes the time and clears the invalid
 * flag. `init` writes the fixed settings once. All return 0 or -errno. */
typedef struct rtc_chip_t {
    int (*init)(const rtc_bus_t *bus);
    int (*get)(const rtc_bus_t *bus, rtc_tm_t *tm, bool *valid);
    int (*set)(const rtc_bus_t *bus, const rtc_tm_t *tm);
} rtc_chip_t;

/* NXP PCF85063A at I2C address 0x51. */
extern const rtc_chip_t RtcChipPcf85063;

/* Initialize `chip` on `bus` (copied) and register it as the device `name`.
 * Errors: -EINVAL a missing function, the RtcDeviceRegister errors, or the
 * chip's init error. The device is registered only on success. */
int RtcChipRegister(const char *name, const rtc_chip_t *chip,
                    const rtc_bus_t *bus);

/* Forget every registered chip. Pair with RtcDevicesReset. */
void RtcChipsReset(void);
