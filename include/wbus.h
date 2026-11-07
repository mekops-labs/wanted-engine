/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

/* The wbus message header, shared by the engine and by wapps that open
 * /dev/wbus/msg. Integers are little-endian; one write or read is one message:
 * this header followed by `len` payload bytes. */

#define WBUS_VERSION 1
#define WBUS_HEADER_SIZE 64

/* Bytes of an object, member or peer field, NUL-padded. A name holds 1 to
 * WBUS_NAME_MAX characters from [A-Za-z0-9_-]. */
#define WBUS_NAME_SIZE 16
#define WBUS_NAME_MAX 15

#define WBUS_MSG_PATH "/dev/wbus/msg"
#define WBUS_STATUS_PATH "/dev/wbus/status"

/* The object only the supervisor instance may serve. */
#define WBUS_SUPERVISOR "supervisor"

enum {
    WBUS_CALL = 1,
    WBUS_REPLY = 2,
    WBUS_EVENT = 3,
    WBUS_SERVE = 4,
    WBUS_UNSERVE = 5,
    WBUS_SUBSCRIBE = 6,
    WBUS_UNSUBSCRIBE = 7,
};

/* `status` of a REPLY is 0 or a WASI errno value. These are the values the
 * engine writes itself, and the one a server answers an unknown method with. */
enum {
    WBUS_STATUS_OK = 0,
    WBUS_STATUS_ENOSYS = 52,
    WBUS_STATUS_EPIPE = 64,
};

typedef struct wbus_header_t {
    uint8_t version;
    uint8_t type;
    uint16_t len;     /* payload bytes */
    uint32_t id;      /* call identifier */
    uint32_t status;  /* REPLY only */
    uint16_t dropped; /* on read: events lost since the previous read */
    uint16_t reserved;
    char object[WBUS_NAME_SIZE];
    char member[WBUS_NAME_SIZE];
    char peer[WBUS_NAME_SIZE]; /* on read: the sender, stamped by the engine */
} wbus_header_t;

_Static_assert(sizeof(wbus_header_t) == WBUS_HEADER_SIZE,
               "wbus header is 64 bytes");
