/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/uart.h"

#include <platform.h>
#include <wanted-autoconf.h>

/* ESP-IDF UART backing over esp_driver_uart. The grant's port= is the UART
 * number; tx= and rx= are GPIO numbers routed through the matrix.
 * uart_driver_install refuses an installed port, giving exclusivity free. */

#define ESP_UART_MAX_PORTS 2

/* Cap on the transmit drain before a line-rate change. A stalled transmitter —
 * flow control asserted by a peer that never releases it — would otherwise hold
 * the wapp for as long as the stall lasts. */
#define ESP_UART_TX_DRAIN_MS 1000

struct platform_uart_t {
    bool used;
    uart_port_t port;
};

static struct platform_uart_t ports[ESP_UART_MAX_PORTS];

static int parseDecimal(const char *s, size_t len, int *out) {
    if (len == 0 || len > 3)
        return -EINVAL;
    int n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -EINVAL;
        n = n * 10 + (s[i] - '0');
    }
    *out = n;
    return 0;
}

/* Extract tx= and rx= from the platform options. Rejects any other key. */
static int parseOptions(const char *options, int *tx, int *rx) {
    *tx = -1;
    *rx = -1;
    if (options == NULL)
        return -EINVAL;

    const char *p = options;
    while (*p != '\0') {
        const char *kv = p;
        while (*p != '\0' && *p != ',')
            p++;
        size_t kvLen = (size_t)(p - kv);
        if (*p == ',')
            p++;
        if (kvLen > 3 && memcmp(kv, "tx=", 3) == 0) {
            if (parseDecimal(kv + 3, kvLen - 3, tx) < 0)
                return -EINVAL;
        } else if (kvLen > 3 && memcmp(kv, "rx=", 3) == 0) {
            if (parseDecimal(kv + 3, kvLen - 3, rx) < 0)
                return -EINVAL;
        } else {
            return -EINVAL;
        }
    }
    return (*tx >= 0 && *rx >= 0) ? 0 : -EINVAL;
}

static int fillLine(const plat_uart_cfg_t *cfg, uart_config_t *uc) {
    memset(uc, 0, sizeof(*uc));
    uc->baud_rate = (int)cfg->baud;

    switch (cfg->databits) {
    case 5:
        uc->data_bits = UART_DATA_5_BITS;
        break;
    case 6:
        uc->data_bits = UART_DATA_6_BITS;
        break;
    case 7:
        uc->data_bits = UART_DATA_7_BITS;
        break;
    case 8:
        uc->data_bits = UART_DATA_8_BITS;
        break;
    default:
        return -EINVAL;
    }

    switch (cfg->parity) {
    case 'N':
        uc->parity = UART_PARITY_DISABLE;
        break;
    case 'E':
        uc->parity = UART_PARITY_EVEN;
        break;
    case 'O':
        uc->parity = UART_PARITY_ODD;
        break;
    default:
        return -EINVAL;
    }

    uc->stop_bits = (cfg->stopbits == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    uc->flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc->source_clk = UART_SCLK_DEFAULT;
    return 0;
}

int PlatformUartOpen(const plat_uart_cfg_t *cfg, platform_uart_t **out) {
    if (cfg == NULL || out == NULL)
        return -EINVAL;

    int num = 0;
    if (parseDecimal(cfg->port, strlen(cfg->port), &num) < 0)
        return -EINVAL;
    if (num < 0 || num >= UART_NUM_MAX)
        return -EINVAL;
    /* UART0 carries the ROM boot log; handing it to a wapp corrupts both. */
    if (num == 0)
        return -EINVAL;

    int tx;
    int rx;
    int rc = parseOptions(cfg->options, &tx, &rx);
    if (rc < 0)
        return rc;

    uart_config_t uc;
    rc = fillLine(cfg, &uc);
    if (rc < 0)
        return rc;

    struct platform_uart_t *slot = NULL;
    for (int i = 0; i < ESP_UART_MAX_PORTS; i++) {
        if (!ports[i].used) {
            slot = &ports[i];
            break;
        }
    }
    if (slot == NULL)
        return -ENOSPC;

    /* Refuses a port already installed, which is the exclusivity guarantee. */
    if (uart_driver_install((uart_port_t)num, CONFIG_WANTED_UART_BUF_SIZE,
                            CONFIG_WANTED_UART_BUF_SIZE, 0, NULL, 0) != ESP_OK)
        return -EBUSY;
    if (uart_param_config((uart_port_t)num, &uc) != ESP_OK) {
        uart_driver_delete((uart_port_t)num);
        return -EINVAL;
    }
    if (uart_set_pin((uart_port_t)num, tx, rx, UART_PIN_NO_CHANGE,
                     UART_PIN_NO_CHANGE) != ESP_OK) {
        uart_driver_delete((uart_port_t)num);
        return -EINVAL;
    }

    slot->used = true;
    slot->port = (uart_port_t)num;
    *out = slot;
    return 0;
}

int PlatformUartConfigure(platform_uart_t *u, const plat_uart_cfg_t *cfg) {
    if (u == NULL || !u->used || cfg == NULL)
        return -EINVAL;

    uart_config_t uc;
    int rc = fillLine(cfg, &uc);
    if (rc < 0)
        return rc;

    /* Let the transmitter finish before the rate changes, so no byte is
     * truncated mid-frame. */
    if (uart_wait_tx_done(u->port, pdMS_TO_TICKS(ESP_UART_TX_DRAIN_MS)) !=
        ESP_OK)
        return -EIO;
    if (uart_param_config(u->port, &uc) != ESP_OK)
        return -EINVAL;
    /* Bytes clocked in under the old settings cannot be decoded now. */
    uart_flush_input(u->port);
    return 0;
}

int PlatformUartRead(platform_uart_t *u, void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    int n = uart_read_bytes(u->port, buf, (uint32_t)nbyte, 0);
    return (n < 0) ? -EIO : n;
}

int PlatformUartWrite(platform_uart_t *u, const void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(u->port, &room) != ESP_OK)
        return -EIO;
    if (room == 0)
        return 0;
    if (nbyte > room)
        nbyte = room;
    int n = uart_write_bytes(u->port, buf, nbyte);
    return (n < 0) ? -EIO : n;
}

void PlatformUartClose(platform_uart_t *u) {
    if (u == NULL || !u->used)
        return;
    uart_driver_delete(u->port);
    memset(u, 0, sizeof(*u));
}
