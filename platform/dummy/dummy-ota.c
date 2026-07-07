/* SPDX-License-Identifier: Apache-2.0 */

/* Unit-test platform: no bootloader to bind to. Reports a single, permanently
 * confirmed slot 'a'; write calls are no-ops that fail, keeping PlatformOta*
 * resolvable in the test build without a real A/B mechanism. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>

int PlatformOtaInit(void) { return 0; }

int PlatformOtaConfirm(void) { return 0; }

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;

    out->active_slot = 'a';
    out->confirmed = true;
    out->pending_swap = false;
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    return 0;
}

int PlatformOtaBeginWrite(void) { return -ENOSYS; }

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int PlatformOtaCommit(void) { return -ENOSYS; }

int PlatformOtaRollback(void) { return -ENOSYS; }
