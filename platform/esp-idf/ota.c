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
#include "esp_timer.h"

#include <platform.h>

/* Must clear the empirical ~5 s supervisor load with headroom, but not leave
 * an operator waiting forever on a hung image -- 3x that measured worst
 * case. */
#define OTA_REVERT_TIMEOUT_US (45U * 1000U * 1000U)

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

static void revertTimerFired(void *arg) {
    (void)arg;
    /* Runs on the esp_timer service task, whose stack is the ordinary
     * internal-DRAM default (only wapp worker threads opt into a PSRAM
     * stack), so this call needs no helper-thread indirection. Does not
     * return on success: marks the running (still-PENDING_VERIFY) slot
     * invalid, reverts the boot partition to the other slot, reboots. */
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

/* Dedicated helper thread, internal-DRAM stack (esp_pthread's plain
 * default): proxies every esp_ota_* call so the cache-freeze safety check
 * never observes a PSRAM-stacked caller, regardless of which thread
 * (wapp worker, main task) invoked the PlatformOta* entry point. */
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

    if (state == ESP_OTA_IMG_PENDING_VERIFY && !g_ota.revertArmed) {
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

static void confirmJobFn(void *arg) {
    confirmJob_t *j = arg;
    esp_ota_img_states_t state;

    if (esp_ota_get_state_partition(g_ota.running, &state) == ESP_OK &&
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        j->rc = 0; /* already confirmed, or nothing pending: no-op */
        return;
    }

    if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
        j->rc = -EIO;
        return;
    }

    if (g_ota.revertArmed) {
        esp_timer_stop(g_ota.revertTimer);
        esp_timer_delete(g_ota.revertTimer);
        g_ota.revertArmed = false;
    }
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
    g_ota.writeTarget = NULL;
    j->rc = (err == ESP_OK) ? 0 : -EIO;
}

static void rollbackJobFn(void *arg) {
    (void)arg;
    /* Does not return on success -- reboots into the other slot. */
    esp_ota_mark_app_invalid_rollback_and_reboot();
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

int PlatformOtaRollback(void) {
    if (!g_ota.inited)
        return -ENODEV;
    otaRunOnHelper(rollbackJobFn, NULL);
    return -EIO; /* only reached on failure (board reboots on success) */
}
