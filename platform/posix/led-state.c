/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

/* State-only LED backing: no output, but a level that follows the monotonic
 * clock through a fade, for hosts and tests. An address is any
 * [A-Za-z0-9_-] name; opening one address twice returns -EBUSY. */

#define LED_STATE_MAX 8
#define LED_STATE_ADDR_MAX 15
#define LED_STATE_PWM_MAX 255U
#define LED_STATE_NS_PER_MS 1000000ULL

struct platform_led_t {
    bool used;
    char address[LED_STATE_ADDR_MAX + 1];
    uint8_t mode;
    unsigned from;
    unsigned to;
    uint64_t startNs;
    uint64_t durNs;
};

static struct platform_led_t leds[LED_STATE_MAX];

static uint64_t nowNs(void) {
    plat_timestamp_t ns = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &ns) < 0)
        return 0;
    return ns;
}

static bool validAddress(const char *a) {
    size_t n = a != NULL ? strlen(a) : 0;
    if (n == 0 || n > LED_STATE_ADDR_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        bool ok = (a[i] >= 'A' && a[i] <= 'Z') ||
                  (a[i] >= 'a' && a[i] <= 'z') ||
                  (a[i] >= '0' && a[i] <= '9') || a[i] == '_' || a[i] == '-';
        if (!ok)
            return false;
    }
    return true;
}

int PlatformLedOpen(const plat_led_cfg_t *cfg, platform_led_t **out) {
    if (cfg == NULL || out == NULL || !validAddress(cfg->address))
        return -EINVAL;

    struct platform_led_t *slot = NULL;
    for (int i = 0; i < LED_STATE_MAX; i++) {
        if (leds[i].used && strcmp(leds[i].address, cfg->address) == 0)
            return -EBUSY;
        if (!leds[i].used && slot == NULL)
            slot = &leds[i];
    }
    if (slot == NULL)
        return -ENOSPC;

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->mode = cfg->mode;
    strncpy(slot->address, cfg->address, LED_STATE_ADDR_MAX);
    *out = slot;
    return 0;
}

unsigned PlatformLedMax(const platform_led_t *l) {
    return (l != NULL && l->mode == PLAT_LED_MODE_PWM) ? LED_STATE_PWM_MAX : 1U;
}

bool PlatformLedHwFade(const platform_led_t *l) {
    return l != NULL && l->mode == PLAT_LED_MODE_PWM;
}

unsigned PlatformLedGet(const platform_led_t *l) {
    if (l == NULL || !l->used)
        return 0;
    uint64_t now = nowNs();
    if (l->durNs == 0 || now >= l->startNs + l->durNs)
        return l->to;
    if (l->mode == PLAT_LED_MODE_ONOFF)
        return l->from;
    int64_t delta = (int64_t)l->to - (int64_t)l->from;
    int64_t elapsed = (int64_t)(now - l->startNs);
    return (unsigned)((int64_t)l->from + delta * elapsed / (int64_t)l->durNs);
}

int PlatformLedSet(platform_led_t *l, unsigned level) {
    if (l == NULL || !l->used || level > PlatformLedMax(l))
        return -EINVAL;
    l->from = level;
    l->to = level;
    l->durNs = 0;
    return 0;
}

int PlatformLedFade(platform_led_t *l, unsigned level, unsigned ms) {
    if (l == NULL || !l->used || level > PlatformLedMax(l))
        return -EINVAL;
    l->from = PlatformLedGet(l);
    l->to = level;
    l->startNs = nowNs();
    l->durNs = (uint64_t)ms * LED_STATE_NS_PER_MS;
    return 0;
}

void PlatformLedClose(platform_led_t *l) {
    if (l != NULL)
        memset(l, 0, sizeof(*l));
}
