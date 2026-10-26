/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

/* State-only power backing: a sleep returns at once as if the timer fired,
 * and the state block lives in memory. */

#define LINUX_POWER_STATE_MAX 512

static uint8_t state[LINUX_POWER_STATE_MAX];
static size_t stateLen;

plat_power_wake_t PlatformPowerLightSleep(uint64_t timerUs) {
    (void)timerUs;
    return PLAT_POWER_WAKE_TIMER;
}

void PlatformPowerDeepSleep(uint64_t timerUs) { (void)timerUs; }

plat_power_boot_t PlatformPowerBootCause(void) { return PLAT_POWER_BOOT_COLD; }

bool PlatformPowerStateStore(const void *buf, size_t len) {
    if (buf == NULL || len > sizeof(state))
        return false;
    memcpy(state, buf, len);
    stateLen = len;
    return true;
}

bool PlatformPowerStateLoad(void *buf, size_t len, uint64_t *elapsedUs) {
    if (buf == NULL || stateLen == 0 || stateLen != len)
        return false;
    memcpy(buf, state, len);
    if (elapsedUs != NULL)
        *elapsedUs = 0;
    return true;
}
