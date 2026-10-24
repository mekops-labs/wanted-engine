/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "dummy-fs.h"
#include <platform.h>

/* A power backing that never sleeps: a sleep returns at once with the wake
 * cause a test set, and the calls are counted. */

#define DUMMY_POWER_STATE_MAX 512

static plat_power_wake_t nextWake;
static plat_power_boot_t bootCause;
static uint64_t bootElapsedUs;
static unsigned lightSleeps;
static unsigned deepSleeps;
static uint64_t lastTimerUs;
static uint8_t state[DUMMY_POWER_STATE_MAX];
static size_t stateLen;
static bool stateValid;

void DummyPowerReset(void) {
    nextWake = PLAT_POWER_WAKE_TIMER;
    bootCause = PLAT_POWER_BOOT_COLD;
    bootElapsedUs = 0;
    lightSleeps = 0;
    deepSleeps = 0;
    lastTimerUs = 0;
    stateLen = 0;
    stateValid = false;
}

void DummyPowerWakeSet(int cause) { nextWake = (plat_power_wake_t)cause; }

void DummyPowerBootSet(int cause, uint64_t elapsedUs) {
    bootCause = (plat_power_boot_t)cause;
    bootElapsedUs = elapsedUs;
}

unsigned DummyPowerLightSleeps(void) { return lightSleeps; }

unsigned DummyPowerDeepSleeps(void) { return deepSleeps; }

uint64_t DummyPowerLastTimerUs(void) { return lastTimerUs; }

plat_power_wake_t PlatformPowerLightSleep(uint64_t timerUs) {
    lightSleeps++;
    lastTimerUs = timerUs;
    return nextWake;
}

void PlatformPowerDeepSleep(uint64_t timerUs) {
    deepSleeps++;
    lastTimerUs = timerUs;
}

plat_power_boot_t PlatformPowerBootCause(void) { return bootCause; }

bool PlatformPowerStateStore(const void *buf, size_t len) {
    if (buf == NULL || len > sizeof(state))
        return false;
    memcpy(state, buf, len);
    stateLen = len;
    stateValid = true;
    return true;
}

bool PlatformPowerStateLoad(void *buf, size_t len, uint64_t *elapsedUs) {
    if (buf == NULL || !stateValid || stateLen != len)
        return false;
    memcpy(buf, state, len);
    if (elapsedUs != NULL)
        *elapsedUs = bootElapsedUs;
    return true;
}
