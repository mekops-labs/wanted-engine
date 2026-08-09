/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* Loopback UART fake for the unit tests: bytes written to `data` land in a ring
 * the same port reads back, exercising blocking and short reads with no peer.
 * Line settings are stored and reported, not enforced. */

#define DUMMY_UART_RING 256

struct platform_uart_t {
    bool used;
    char port[16];
    uint8_t ring[DUMMY_UART_RING];
    size_t rpos;
    size_t len;
    uint32_t baud;
    uint8_t databits;
    uint8_t parity;
    uint8_t stopbits;
};

static struct platform_uart_t port;

void DummyUartReset(void) { memset(&port, 0, sizeof(port)); }

int DummyUartGetLine(uint32_t *baud, uint8_t *databits, uint8_t *parity,
                     uint8_t *stopbits) {
    if (!port.used)
        return -ENOENT;
    if (baud)
        *baud = port.baud;
    if (databits)
        *databits = port.databits;
    if (parity)
        *parity = port.parity;
    if (stopbits)
        *stopbits = port.stopbits;
    return 0;
}

int DummyUartRxLen(void) { return port.used ? (int)port.len : -ENOENT; }

static void adopt(struct platform_uart_t *u, const plat_uart_cfg_t *cfg) {
    u->baud = cfg->baud;
    u->databits = cfg->databits;
    u->parity = cfg->parity;
    u->stopbits = cfg->stopbits;
}

int PlatformUartOpen(const plat_uart_cfg_t *cfg, platform_uart_t **out) {
    if (cfg == NULL || cfg->port == NULL || out == NULL)
        return -EINVAL;
    if (strlen(cfg->port) >= sizeof(port.port))
        return -EINVAL;
    if (port.used)
        return -EBUSY;

    memset(&port, 0, sizeof(port));
    strncpy(port.port, cfg->port, sizeof(port.port) - 1);
    adopt(&port, cfg);
    port.used = true;
    *out = &port;
    return 0;
}

int PlatformUartConfigure(platform_uart_t *u, const plat_uart_cfg_t *cfg) {
    if (u == NULL || !u->used || cfg == NULL)
        return -EINVAL;
    /* A rate no real backing produces is refused here too, so a test can cover
     * the -EINVAL path the driver must surface. */
    if (cfg->baud < 300 || cfg->baud > 3000000)
        return -EINVAL;
    adopt(u, cfg);
    u->rpos = 0;
    u->len = 0; /* the receive ring is discarded on reconfiguration */
    return 0;
}

int PlatformUartRead(platform_uart_t *u, void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    if (u->len == 0 || nbyte == 0)
        return 0;

    size_t n = (nbyte < u->len) ? nbyte : u->len;
    uint8_t *dst = buf;
    for (size_t i = 0; i < n; i++)
        dst[i] = u->ring[(u->rpos + i) % DUMMY_UART_RING];
    u->rpos = (u->rpos + n) % DUMMY_UART_RING;
    u->len -= n;
    return (int)n;
}

int PlatformUartWrite(platform_uart_t *u, const void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    size_t room = DUMMY_UART_RING - u->len;
    if (room == 0 || nbyte == 0)
        return 0;

    size_t n = (nbyte < room) ? nbyte : room;
    const uint8_t *src = buf;
    for (size_t i = 0; i < n; i++)
        u->ring[(u->rpos + u->len + i) % DUMMY_UART_RING] = src[i];
    u->len += n;
    return (int)n;
}

void PlatformUartClose(platform_uart_t *u) {
    if (u == NULL || !u->used)
        return;
    memset(u, 0, sizeof(*u));
}
