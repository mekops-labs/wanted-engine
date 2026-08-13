/* SPDX-License-Identifier: Apache-2.0 */

/* Wake descriptors backed by eventfd. This platform implements no pthread_kill,
 * so a worker parked in a host call is reached by making the wait watch this
 * descriptor as well. Registered once, before any wapp launches. */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include <debug_trace.h>
#include <esp_vfs_eventfd.h>
#include <platform.h>

/* One per wapp, plus headroom for a wapp holding more than one wait. */
#define WAKE_MAX_FDS (CONFIG_WANTED_MAX_WAPPS * 2)

static bool registered;

static int ensureRegistered(void) {
    if (registered) {
        return 0;
    }
    esp_vfs_eventfd_config_t cfg = {.max_fds = WAKE_MAX_FDS};
    esp_err_t err = esp_vfs_eventfd_register(&cfg);
    /* Already registered is success: the engine may create a wake descriptor
     * before and after a supervisor reload. */
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        DEBUG_TRACE("eventfd register failed: %d", (int)err);
        return -EIO;
    }
    registered = true;
    return 0;
}

int PlatformWakeCreate(void) {
    if (ensureRegistered() < 0) {
        return -1;
    }
    int fd = eventfd(0, 0);
    if (fd < 0) {
        DEBUG_TRACE("eventfd create failed: %d", errno);
        return -1;
    }
    return fd;
}

void PlatformWakeRaise(int fd) {
    if (fd < 0) {
        return;
    }
    /* A counter write; a reader that never drains it keeps the descriptor
     * readable, which is what an interrupted wait wants. */
    uint64_t one = 1;
    if (write(fd, &one, sizeof(one)) < 0) {
        DEBUG_TRACE("eventfd raise failed: %d", errno);
    }
}

void PlatformWakeClose(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}
