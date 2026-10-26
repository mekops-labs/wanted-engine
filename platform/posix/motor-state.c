/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

/* State-only motor backing: no output, but a pattern stays busy for its
 * length on the monotonic clock, for hosts and tests. An address is any
 * [A-Za-z0-9_-] name; opening one address twice returns -EBUSY. */

#define MOTOR_STATE_MAX 4
#define MOTOR_STATE_ADDR_MAX 15
#define MOTOR_STATE_SEGMENTS_MAX 8
#define MOTOR_STATE_NS_PER_MS 1000000ULL

struct platform_motor_t {
    bool used;
    char address[MOTOR_STATE_ADDR_MAX + 1];
    uint64_t startNs;
    uint64_t durNs;
};

static struct platform_motor_t motors[MOTOR_STATE_MAX];

static uint64_t nowNs(void) {
    plat_timestamp_t ns = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &ns) < 0)
        return 0;
    return ns;
}

static bool validAddress(const char *a) {
    size_t n = a != NULL ? strlen(a) : 0;
    if (n == 0 || n > MOTOR_STATE_ADDR_MAX)
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

int PlatformMotorOpen(const char *address, platform_motor_t **out) {
    if (out == NULL || !validAddress(address))
        return -EINVAL;

    struct platform_motor_t *slot = NULL;
    for (int i = 0; i < MOTOR_STATE_MAX; i++) {
        if (motors[i].used && strcmp(motors[i].address, address) == 0)
            return -EBUSY;
        if (!motors[i].used && slot == NULL)
            slot = &motors[i];
    }
    if (slot == NULL)
        return -ENOSPC;

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    strncpy(slot->address, address, MOTOR_STATE_ADDR_MAX);
    *out = slot;
    return 0;
}

int PlatformMotorPattern(platform_motor_t *m, const uint16_t *ms, size_t n) {
    if (m == NULL || !m->used || ms == NULL || n == 0 ||
        n > MOTOR_STATE_SEGMENTS_MAX)
        return -EINVAL;
    uint64_t total = 0;
    for (size_t i = 0; i < n; i++)
        total += ms[i];
    m->startNs = nowNs();
    m->durNs = total * MOTOR_STATE_NS_PER_MS;
    return 0;
}

void PlatformMotorStop(platform_motor_t *m) {
    if (m != NULL)
        m->durNs = 0;
}

bool PlatformMotorBusy(const platform_motor_t *m) {
    return m != NULL && m->used && nowNs() < m->startNs + m->durNs;
}

void PlatformMotorClose(platform_motor_t *m) {
    if (m != NULL)
        memset(m, 0, sizeof(*m));
}
