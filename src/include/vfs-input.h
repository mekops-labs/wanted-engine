/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

#define EV_SYN 0x00
#define EV_KEY 0x01
#define EV_REL 0x02
#define EV_TEXT 0xF0

#define SYN_DROPPED 3

/* Device capabilities, in the order info lists them. */
#define INPUT_TYPE_KEY 0x1
#define INPUT_TYPE_TEXT 0x2
#define INPUT_TYPE_REL 0x4

#define INPUT_NAME_MAX 15
#define INPUT_QUEUE_LEN 64
#define INPUT_EVENT_BYTES 8

/* One event, 8 bytes little-endian on the wire. */
typedef struct wanted_input_event {
    uint8_t sync; /* 1 on the last event of a batch */
    uint8_t type; /* EV_* */
    uint16_t code;
    int32_t value;
} wanted_input_event_t;

typedef struct input_device_desc_t {
    const char *name; /* [A-Za-z0-9_-], at most INPUT_NAME_MAX */
    uint8_t types;    /* INPUT_TYPE_* mask, at least one */
    const char *keymap;
} input_device_desc_t;

typedef struct input_device_t input_device_t;

/* Add a device to the engine-wide table. Declaring an identical device again
 * returns 0. Errors: -EEXIST other properties, -EINVAL bad name, types or
 * keymap, -ENOSPC table full. `out` may be NULL. */
int InputDeviceRegister(const input_device_desc_t *desc, input_device_t **out);

/* Queue `ev` for the device's owner. A full queue is cleared and holds one
 * SYN_DROPPED record instead; the event that overflowed it is lost. */
void InputDevicePush(input_device_t *dev, const wanted_input_event_t *ev);

/* Drop every device. Only valid while no input driver instance exists. */
void InputDevicesReset(void);
