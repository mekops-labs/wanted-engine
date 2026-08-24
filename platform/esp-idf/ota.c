/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF binding of the shared PlatformOta* seam to the ESP-IDF native OTA
 * subsystem (esp_ota_ops + bootloader app-rollback). Slots are reported as
 * 'a'/'b' regardless of underlying partition subtype */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <platform.h>

/* Must clear the empirical ~5 s supervisor load with headroom, but not leave
 * an operator waiting forever on a hung image -- 3x that measured worst
 * case. */
#define OTA_REVERT_TIMEOUT_US (45U * 1000U * 1000U)

/* Records the slot a staged image awaits its first boot in, because an older
 * bootloader grants no probation to read back. One image writes it and the
 * next reads it, so it is keyed by name: an address in RTC memory moves
 * between two builds, which is exactly the pair an update spans. */
#define OTA_NVS_NAMESPACE "wanted_ota"
#define OTA_NVS_STAGED_KEY "staged_slot"

static struct {
    bool inited;
    const esp_partition_t *running;
    /* Valid only while a write is in flight (BeginWrite..Commit). */
    const esp_partition_t *writeTarget;
    esp_ota_handle_t writeHandle;
    bool writing;
    esp_timer_handle_t revertTimer;
    bool revertArmed;
} g_ota;

static char slotLetter(const esp_partition_t *part) {
    if (part == NULL)
        return '\0';
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0)
        return 'a';
    if (part->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)
        return 'b';
    return '\0';
}

/* NUL-terminated lowercase hex of `len` bytes; `out` holds 2*len+1. */
static void hexEncode(const uint8_t *in, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

/* The OTA seam comes up before whoever else initialises NVS, and init is
 * idempotent. Primed from PlatformOtaInit on the helper thread, before any
 * timer can reach the accessors below. A partition wanting an erase is not
 * this module's to erase: the marker is then simply unavailable. */
static bool otaNvsReady(void) {
    static bool tried, ready;

    if (!tried) {
        tried = true;
        ready = nvs_flash_init() == ESP_OK;
    }
    return ready;
}

static void stagedSlotSet(uint32_t subtype) {
    nvs_handle_t h;

    if (!otaNvsReady() ||
        nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_set_u32(h, OTA_NVS_STAGED_KEY, subtype) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

static void stagedSlotClear(void) {
    nvs_handle_t h;

    if (!otaNvsReady() ||
        nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (nvs_erase_key(h, OTA_NVS_STAGED_KEY) == ESP_OK)
        nvs_commit(h);
    nvs_close(h);
}

/* True with *out set to the slot a staged image is waiting in. */
static bool stagedSlotGet(uint32_t *out) {
    nvs_handle_t h;

    if (!otaNvsReady() ||
        nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK)
        return false;
    bool found = nvs_get_u32(h, OTA_NVS_STAGED_KEY, out) == ESP_OK;
    nvs_close(h);
    return found;
}

/* Boot the slot the running image displaced. Does not return on success.
 * Cancelling probation is the bootloader's path; where it granted none there
 * is nothing to invalidate, so aim the next boot by hand. */
static void otaRevertToOtherSlot(void) {
    esp_ota_mark_app_invalid_rollback_and_reboot();

    const esp_partition_t *previous = esp_ota_get_next_update_partition(NULL);
    if (previous == NULL)
        return;
    if (esp_ota_set_boot_partition(previous) != ESP_OK)
        return;
    stagedSlotClear();
    esp_restart();
}

static void revertTimerFired(void *arg) {
    (void)arg;
    /* The esp_timer service task's stack is internal DRAM, so this needs no
     * helper-thread indirection. */
    otaRevertToOtherSlot();
}

/* Dedicated helper thread with an internal-DRAM stack, proxying every esp_ota_*
 * call so the cache-freeze safety check never observes a PSRAM-stacked caller,
 * whichever thread invoked the PlatformOta* entry point. */
typedef void (*ota_job_fn_t)(void *arg);

static pthread_mutex_t g_helperLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_helperThread;
static bool g_helperStarted;
static sem_t g_jobReady;
static sem_t g_jobDone;
static ota_job_fn_t g_jobFn;
static void *g_jobArg;

static void *otaHelperMain(void *arg) {
    (void)arg;
    for (;;) {
        sem_wait(&g_jobReady);
        g_jobFn(g_jobArg);
        sem_post(&g_jobDone);
    }
    return NULL;
}

static bool otaHelperEnsureStarted(void) {
    if (g_helperStarted)
        return true;
    if (sem_init(&g_jobReady, 0, 0) != 0)
        return false;
    if (sem_init(&g_jobDone, 0, 0) != 0)
        return false;
    if (pthread_create(&g_helperThread, NULL, otaHelperMain, NULL) != 0)
        return false;
    g_helperStarted = true;
    return true;
}

/* Runs `fn(arg)` on the helper thread and blocks until it completes. Returns
 * false only if the helper thread itself could not be started (out of
 * memory); `fn` communicates its own result through `arg`. */
static bool otaRunOnHelper(ota_job_fn_t fn, void *arg) {
    bool ok;

    pthread_mutex_lock(&g_helperLock);
    ok = otaHelperEnsureStarted();
    if (ok) {
        g_jobFn = fn;
        g_jobArg = arg;
        sem_post(&g_jobReady);
        sem_wait(&g_jobDone);
    }
    pthread_mutex_unlock(&g_helperLock);
    return ok;
}

/* --- Job bodies: run only on the helper thread via otaRunOnHelper. --- */

typedef struct {
    int rc;
} initJob_t;

static void initJobFn(void *arg) {
    initJob_t *j = arg;

    g_ota.running = esp_ota_get_running_partition();
    if (g_ota.running == NULL) {
        j->rc = -ENODEV;
        return;
    }

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(g_ota.running, &state) != ESP_OK)
        state = ESP_OTA_IMG_UNDEFINED;

    /* The marker names the slot it was staged into, so a boot of the image
     * this one displaced never arms a revert against a working slot. */
    uint32_t staged;
    bool awaitingFirstBoot =
        stagedSlotGet(&staged) && staged == g_ota.running->subtype;

    if ((state == ESP_OTA_IMG_PENDING_VERIFY || awaitingFirstBoot) &&
        !g_ota.revertArmed) {
        esp_timer_create_args_t args;
        memset(&args, 0, sizeof(args));
        args.callback = revertTimerFired;
        args.dispatch_method = ESP_TIMER_TASK;
        args.name = "ota-revert";
        if (esp_timer_create(&args, &g_ota.revertTimer) == ESP_OK &&
            esp_timer_start_once(g_ota.revertTimer, OTA_REVERT_TIMEOUT_US) ==
                ESP_OK)
            g_ota.revertArmed = true;
    }

    g_ota.inited = true;
    j->rc = 0;
}

typedef struct {
    int rc;
} confirmJob_t;

/* Retire the engine's own probation: the running image is good. */
static void otaRevertDisarm(void) {
    stagedSlotClear();
    if (g_ota.revertArmed) {
        esp_timer_stop(g_ota.revertTimer);
        esp_timer_delete(g_ota.revertTimer);
        g_ota.revertArmed = false;
    }
}

static void confirmJobFn(void *arg) {
    confirmJob_t *j = arg;
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(g_ota.running, &state) == ESP_OK &&
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        otaRevertDisarm(); /* already confirmed, or nothing pending */
        j->rc = 0;
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        j->rc = -EIO;
        return;
    }

    otaRevertDisarm();
    j->rc = 0;
}

typedef struct {
    platform_ota_state_t *out;
    int rc;
} getStateJob_t;

static void getStateJobFn(void *arg) {
    getStateJob_t *j = arg;
    platform_ota_state_t *out = j->out;
    esp_ota_img_states_t state;

    memset(out, 0, sizeof(*out));
    out->active_slot = slotLetter(g_ota.running);

    if (esp_ota_get_state_partition(g_ota.running, &state) != ESP_OK)
        state = ESP_OTA_IMG_UNDEFINED;
    out->confirmed =
        (state == ESP_OTA_IMG_VALID || state == ESP_OTA_IMG_UNDEFINED);
    /* ESP-IDF's native rollback is one-shot -- there is no boot-attempt
     * counter to read, only whether the slot is still on probation. */
    out->boot_attempts = (state == ESP_OTA_IMG_PENDING_VERIFY) ? 1 : 0;

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next != NULL) {
        esp_ota_img_states_t nextState;
        if (esp_ota_get_state_partition(next, &nextState) == ESP_OK) {
            out->pending_swap = (nextState == ESP_OTA_IMG_NEW);
            if (nextState == ESP_OTA_IMG_INVALID ||
                nextState == ESP_OTA_IMG_ABORTED)
                out->last_failed_slot = slotLetter(next);
        }
        /* Only a scheduled slot holds an image worth naming: the inactive slot
         * otherwise carries whatever an older or abandoned write left there. */
        if (out->pending_swap) {
            esp_app_desc_t desc;
            if (esp_ota_get_partition_description(next, &desc) == ESP_OK)
                hexEncode(desc.app_elf_sha256, sizeof(desc.app_elf_sha256),
                          out->pending_digest);
        }
    }
    j->rc = 0;
}

typedef struct {
    int rc;
} beginJob_t;

static void beginJobFn(void *arg) {
    beginJob_t *j = arg;

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        j->rc = -ENODEV;
        return;
    }

    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &g_ota.writeHandle);
    if (err != ESP_OK) {
        j->rc = (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) ? -EPERM : -EIO;
        return;
    }

    g_ota.writeTarget = target;
    g_ota.writing = true;
    j->rc = 0;
}

typedef struct {
    const uint8_t *buf;
    size_t len;
    int rc;
} writeJob_t;

static void writeJobFn(void *arg) {
    writeJob_t *j = arg;
    esp_err_t err = esp_ota_write(g_ota.writeHandle, j->buf, j->len);

    if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        j->rc = -EBADMSG;
    else
        j->rc = (err == ESP_OK) ? 0 : -EIO;
}

typedef struct {
    int rc;
} commitJob_t;

static void commitJobFn(void *arg) {
    commitJob_t *j = arg;
    esp_err_t err = esp_ota_end(g_ota.writeHandle);

    g_ota.writing = false;
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) {
        j->rc = -EBADMSG;
        g_ota.writeTarget = NULL;
        return;
    }
    if (err != ESP_OK) {
        j->rc = -EIO;
        g_ota.writeTarget = NULL;
        return;
    }

    err = esp_ota_set_boot_partition(g_ota.writeTarget);
    if (err == ESP_OK) {
        stagedSlotSet(g_ota.writeTarget->subtype);
    }
    g_ota.writeTarget = NULL;
    j->rc = (err == ESP_OK) ? 0 : -EIO;
}

typedef struct {
    int rc;
} abortJob_t;

static void abortJobFn(void *arg) {
    abortJob_t *j = arg;

    if (!g_ota.writing) {
        j->rc = 0;
        return;
    }

    esp_err_t err = esp_ota_abort(g_ota.writeHandle);
    /* Release the session whatever the driver reports: holding it wedges every
     * later write. */
    g_ota.writing = false;
    g_ota.writeTarget = NULL;
    j->rc = (err == ESP_OK) ? 0 : -EIO;
}

static void rollbackJobFn(void *arg) {
    (void)arg;
    /* Does not return on success -- reboots into the other slot. */
    otaRevertToOtherSlot();
}

/* --- Public PlatformOta* seam: each call is a single round-trip to the
 * helper thread. --- */

int PlatformOtaInit(void) {
    initJob_t j = {0};
    if (!otaRunOnHelper(initJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaConfirm(void) {
    confirmJob_t j = {0};
    if (!g_ota.inited)
        return -ENODEV;
    if (!otaRunOnHelper(confirmJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    getStateJob_t j = {.out = out};
    if (out == NULL)
        return -EINVAL;
    if (!g_ota.inited)
        return -ENODEV;
    if (!otaRunOnHelper(getStateJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaBeginWrite(void) {
    beginJob_t j = {0};
    if (!g_ota.inited)
        return -ENODEV;
    if (g_ota.writing)
        return -EBUSY;
    if (!otaRunOnHelper(beginJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    writeJob_t j = {.buf = buf, .len = len};
    if (!g_ota.writing)
        return -EPERM;
    if (buf == NULL && len > 0)
        return -EINVAL;
    if (!otaRunOnHelper(writeJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaCommit(void) {
    commitJob_t j = {0};
    if (!g_ota.writing)
        return -EPERM;
    if (!otaRunOnHelper(commitJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaAbort(void) {
    abortJob_t j = {0};
    if (!g_ota.inited)
        return -ENODEV;
    if (!otaRunOnHelper(abortJobFn, &j))
        return -ENOMEM;
    return j.rc;
}

int PlatformOtaRollback(void) {
    if (!g_ota.inited)
        return -ENODEV;
    otaRunOnHelper(rollbackJobFn, NULL);
    return -EIO; /* only reached on failure (board reboots on success) */
}
