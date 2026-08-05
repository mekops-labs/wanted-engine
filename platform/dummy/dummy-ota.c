/* SPDX-License-Identifier: Apache-2.0 */

/* In-memory A/B fake: the same state machine the host backing keeps in files,
 * so the driver's contract is testable without a bootloader. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

#define DUMMY_OTA_SLOT_MAX 512

static struct {
    char active;
    char pending; /* staged slot awaiting confirm, 0 when none */
    bool writing;
    uint8_t image[DUMMY_OTA_SLOT_MAX];
    size_t len;
    size_t committed_len;
} g;

void DummyOtaReset(void) {
    memset(&g, 0, sizeof(g));
    g.active = 'a';
}

char DummyOtaActiveSlot(void) { return g.active != 0 ? g.active : 'a'; }

int DummyOtaStagedLen(void) {
    return g.pending != 0 ? (int)g.committed_len : -1;
}

static char inactive(void) { return DummyOtaActiveSlot() == 'a' ? 'b' : 'a'; }

int PlatformOtaInit(void) {
    if (g.active == 0)
        g.active = 'a';
    return 0;
}

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;

    out->active_slot = DummyOtaActiveSlot();
    out->pending_swap = g.pending != 0;
    out->confirmed = g.pending == 0;
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    return 0;
}

int PlatformOtaBeginWrite(void) {
    if (g.writing)
        return -EBUSY;
    g.writing = true;
    g.len = 0;
    return 0;
}

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    if (!g.writing)
        return -EPERM;
    if (buf == NULL)
        return -EINVAL;
    if (g.len + len > sizeof(g.image))
        return -ENOSPC;
    memcpy(g.image + g.len, buf, len);
    g.len += len;
    return 0;
}

int PlatformOtaCommit(void) {
    if (!g.writing)
        return -EPERM;
    g.writing = false;
    g.committed_len = g.len;
    g.pending = inactive();
    return 0;
}

int PlatformOtaAbort(void) {
    g.writing = false;
    g.len = 0;
    return 0;
}

int PlatformOtaConfirm(void) {
    if (g.pending == 0)
        return 0;
    g.active = g.pending;
    g.pending = 0;
    return 0;
}

int PlatformOtaRollback(void) {
    g.pending = 0;
    return 0;
}
