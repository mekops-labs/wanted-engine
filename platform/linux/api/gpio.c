/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <platform.h>

/* Linux has no GPIO backing yet. The real one is libgpiod v2 over
 * /dev/gpiochipN, which takes a "chip:offset" address; the legacy sysfs GPIO
 * interface is removed from current kernels and is not an option.
 *
 * Returning -ENOSYS fails a launch that grants `gpio` on this target. A wapp
 * therefore never reaches a line the host did not configure. */

int PlatformGpioOpen(const plat_gpio_cfg_t *cfg, platform_gpio_t **out) {
    (void)cfg;
    (void)out;
    return -ENOSYS;
}

/* `level` is an out-parameter of the shared signature. This stub never
 * writes it, which is the only reason it reads as const-able here. */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
int PlatformGpioRead(const platform_gpio_t *g, bool *level) {
    (void)g;
    (void)level;
    return -ENOSYS;
}

int PlatformGpioWrite(platform_gpio_t *g, bool level) {
    (void)g;
    (void)level;
    return -ENOSYS;
}

void PlatformGpioClose(platform_gpio_t *g) { (void)g; }
