/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <platform.h>

/* NuttX has no UART backing. The port would be a /dev/ttySN character device
 * driven through termios, the same shape as the Linux backing. Returning
 * -ENOSYS fails a launch that grants `uart` on this target. */

int PlatformUartOpen(const plat_uart_cfg_t *cfg, platform_uart_t **out) {
    (void)cfg;
    (void)out;
    return -ENOSYS;
}

int PlatformUartConfigure(platform_uart_t *u, const plat_uart_cfg_t *cfg) {
    (void)u;
    (void)cfg;
    return -ENOSYS;
}

int PlatformUartRead(platform_uart_t *u, void *buf, size_t nbyte) {
    (void)u;
    (void)buf;
    (void)nbyte;
    return -ENOSYS;
}

int PlatformUartWrite(platform_uart_t *u, const void *buf, size_t nbyte) {
    (void)u;
    (void)buf;
    (void)nbyte;
    return -ENOSYS;
}

void PlatformUartClose(platform_uart_t *u) { (void)u; }
