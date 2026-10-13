/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LORA_NAME_MAX 15
#define LORA_PAYLOAD_MAX 255
#define LORA_RECORD_HEADER 20
#define LORA_MAX_BANDS 8

/* Modem settings, applied whole. Frames always carry an explicit header and a
 * CRC. `sync` is the 1-byte LoRa sync word; the backing converts it. */
typedef struct lora_params_t {
    uint32_t freq_hz;
    uint32_t bw_hz;
    uint8_t sf; /* 5..12 */
    uint8_t cr; /* coding rate denominator, 5..8 */
    uint8_t sync;
    int8_t power_dbm;  /* already clamped to the band's limit */
    uint16_t preamble; /* symbols */
} lora_params_t;

/* Every call gets the device's `ctx` and returns 0 or a negative errno.
 * `transmit` and `channel_active` are optional. */
typedef struct lora_ops_t {
    /* Runs in standby; on -EINVAL the chip keeps its settings. */
    int (*configure)(void *ctx, const lora_params_t *p);
    int (*start)(void *ctx);
    int (*stop)(void *ctx);
    int (*sleep)(void *ctx);
    /* Blocks until the frame is sent; leaves the chip in standby. */
    int (*transmit)(void *ctx, const uint8_t *buf, size_t len);
    int (*channel_active)(void *ctx, bool *active);
} lora_ops_t;

/* A band the board may transmit in; `duty_permille` is the share of an hour on
 * air, in thousandths. */
typedef struct lora_band_t {
    uint32_t lo_hz;
    uint32_t hi_hz;
    int8_t max_dbm;
    uint16_t duty_permille;
} lora_band_t;

typedef struct lora_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most LORA_NAME_MAX */
    const char *chip; /* shown on status */
    const lora_ops_t *ops;
    void *ctx;
    const lora_band_t *bands; /* NULL or empty: receive only */
    uint8_t band_count;       /* at most LORA_MAX_BANDS */
} lora_device_desc_t;

/* A received frame as the backing reads it from the chip. */
typedef struct lora_rx_frame_t {
    const uint8_t *payload;
    size_t len;
    int16_t rssi_ddbm; /* tenths of a dBm */
    int16_t snr_ddb;   /* tenths of a dB */
    bool crc_ok;
} lora_rx_frame_t;

typedef struct lora_device_t lora_device_t;

/* Add a board device to the engine-wide table. Errors: -EINVAL bad
 * description, -EEXIST name taken, -ENOSPC table full. `out` may be NULL. */
int LoraDeviceRegister(const lora_device_desc_t *desc, lora_device_t **out);

lora_device_t *LoraDeviceFind(const char *name);

/* Queue a frame for the owner. Kept only while the radio receives and the CRC
 * was good; a full queue drops its oldest frame. Safe from a backing thread. */
void LoraDevicePush(lora_device_t *dev, const lora_rx_frame_t *f);

/* Time on air of a frame of `len` bytes, in microseconds (SX126x and LR11xx
 * data sheets, explicit header and CRC). */
uint32_t LoraTimeOnAirUs(const lora_params_t *p, size_t len);

/* Low data rate optimization, on when a symbol lasts 16 ms or more. A backing
 * sets the chip from this, so the time on air matches. */
bool LoraLowDataRate(const lora_params_t *p);

/* Drop every device. Only valid while no lora driver instance exists. */
void LoraDevicesReset(void);
