/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <rtc-chip.h>
#include <vfs-rtc.h>
#include <wanted-autoconf.h>

/* Adapts a chip over a bus to the device a grant reads and writes. */

typedef struct chip_dev_t {
    const rtc_chip_t *chip;
    rtc_bus_t bus;
} chip_dev_t;

static chip_dev_t pool[CONFIG_WANTED_RTC_MAX_DEVICES];
static size_t poolUsed;

static int chipGet(void *ctx, uint32_t *sec, bool *valid) {
    const chip_dev_t *d = ctx;
    rtc_tm_t tm;
    bool chipValid = false;
    int rc = d->chip->get(&d->bus, &tm, &chipValid);
    if (rc < 0)
        return rc;
    /* Registers that decode to no calendar time cannot be vouched for. */
    *valid = chipValid && RtcTmToUnix(&tm, sec) == 0;
    return 0;
}

static int chipSet(void *ctx, uint32_t sec) {
    const chip_dev_t *d = ctx;
    rtc_tm_t tm;
    int rc = RtcUnixToTm(sec, &tm);
    if (rc < 0)
        return rc;
    return d->chip->set(&d->bus, &tm);
}

static const rtc_ops_t chipOps = {chipGet, chipSet};

int RtcChipRegister(const char *name, const rtc_chip_t *chip,
                    const rtc_bus_t *bus) {
    if (name == NULL || chip == NULL || chip->init == NULL ||
        chip->get == NULL || chip->set == NULL || bus == NULL ||
        bus->read == NULL || bus->write == NULL)
        return -EINVAL;
    if (poolUsed >= CONFIG_WANTED_RTC_MAX_DEVICES)
        return -ENOSPC;

    chip_dev_t *d = &pool[poolUsed];
    d->chip = chip;
    d->bus = *bus;
    int rc = chip->init(&d->bus);
    if (rc < 0)
        return rc;
    rtc_device_desc_t desc = {name, &chipOps, d};
    rc = RtcDeviceRegister(&desc);
    if (rc < 0)
        return rc;
    poolUsed++;
    return 0;
}

void RtcChipsReset(void) { poolUsed = 0; }
