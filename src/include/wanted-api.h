/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted-autoconf.h>
#include <wanted-host-guard.h>
#include <wanted.h>

/* Per-wapp log ring slots. Derived rather than configured: a slot exists for
 * each wapp the engine can run, so the two counts cannot disagree. */
#define CONFIG_WANTED_LOG_SLOTS CONFIG_WANTED_MAX_WAPPS

/* Version of the contract between the engine and a supervisor wapp: the shape
 * of the /dev/wanted control plane, its verbs, and the wapp states it reports.
 * Bump only when a supervisor built against an earlier value would misread. */
#define WANTED_SUPERVISOR_ABI 1

#define WAPP_MAX_NAME_LEN 15
#define WAPP_MAX_VERSION_LEN 15
#define MAX_DRIVER_NAME 15
/* CONFIG_WANTED_MAX_DRIVERS_CNT and CONFIG_WANTED_MAX_OPTIONS_SIZE are
 * profile-tunable footprint knobs and live with the other resource limits in
 * the Kconfig tree. */

/* An image reference is "<name>:<tag>" — an image name plus an optional version
 * tag. It bounds the config `image` field, which may carry a pinned tag. */
#define WAPP_MAX_IMAGE_REF_LEN (WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN)

/* Command-line arguments and environment variables passed to a wapp via its
 * launch config. argv[0] is the wapp name (set by the engine at launch), so
 * these arrays hold argv[1..] and the POSIX "KEY=VALUE" environ entries. */
#define WAPP_MAX_ARGS 8
#define WAPP_MAX_ARG_LEN 64
#ifndef WAPP_MAX_ENVS
#define WAPP_MAX_ENVS 8
#endif
/* Sized to carry a "KEY=<64 hex chars>" entry (provisioning passes key material
 * through the environment). Cost is WAPP_MAX_ENVS * this, per wapp. */
#ifndef WAPP_MAX_ENV_LEN
#define WAPP_MAX_ENV_LEN 128
#endif

/* Sentinel exit code for a wapp that has no WASI exit code: a wapp that traps
 * never reaches proc_exit, and a wapp that has not exited yet has no code. The
 * exit_code field is authoritative only when status == EXITED. */
#define WAPP_EXIT_CODE_NONE (-1)

struct wamrData_t;

/* A launch-config resource entry, shared by drivers[], mounts[] and sockets[]
 * with per-section rules — see the configuration reference. A field a section
 * forbids is rejected at install time. */
typedef struct wapp_driver_t {
    char name[WAPP_MAX_NAME_LEN];
    char path[CONFIG_WANTED_MAX_PATH_LEN];
    char options[CONFIG_WANTED_MAX_OPTIONS_SIZE];
} wapp_driver_t;

typedef struct wapp_config_t {
    bool valid;
    /* Registry image this instance runs, as "<name>[:<tag>]"; an absent tag
     * is a first-match. Empty means the instance name, so N instances share one
     * image while a single-instance wapp need never set it. */
    char image[WAPP_MAX_IMAGE_REF_LEN];
    wapp_driver_t console[3];
    /* Device singletons, mounted at "/dev/<name>". */
    size_t driversCnt;
    wapp_driver_t drivers[CONFIG_WANTED_MAX_DRIVERS_CNT];
    /* File/backend drivers bound at an arbitrary absolute path. */
    size_t mountsCnt;
    wapp_driver_t mounts[CONFIG_WANTED_MAX_DRIVERS_CNT];
    /* Named sockets created at "/net/<name>". */
    size_t socketsCnt;
    wapp_driver_t sockets[CONFIG_WANTED_MAX_DRIVERS_CNT];
    char args[WAPP_MAX_ARGS][WAPP_MAX_ARG_LEN];
    size_t argsCnt;
    char envs[WAPP_MAX_ENVS][WAPP_MAX_ENV_LEN];
    size_t envsCnt;
} wapp_config_t;

typedef enum wapp_action_t {
    WAPP_START,
    WAPP_STOP,
} wapp_action_t;

typedef struct wapp_t {
    char name[WAPP_MAX_NAME_LEN];  /* instance identity (set at launch) */
    char image[WAPP_MAX_NAME_LEN]; /* registry image identity (set by loader) */
    char version[WAPP_MAX_VERSION_LEN]; /* image version tag (set by loader) */
    wapp_config_t cfg;
    /* OCI layer stack (newest first, index 0 = topmost) */
    uint8_t *layers[TARFS_MAX_LAYERS];
    size_t layer_lens[TARFS_MAX_LAYERS];
    uint8_t layer_cnt; /* must be >= 1 for a valid wapp */
} wapp_t;

typedef struct wapp_data_t {
    uint8_t id;
    wapp_t *wapp;
    vfs_ctx_t vfs;
    struct wamrData_t *wamr;
    int lastStatus;
    /* WASI exit code from the run, or WAPP_EXIT_CODE_NONE on a trap.
     * Authoritative only when the slot's status is EXITED. Lives in each
     * platform's persistent wapp slot, so it survives the run. */
    int32_t exit_code;
} wapp_data_t;

typedef enum status_t {
    NOT_STARTED,
    CREATED, /* namespace reserved via `create`, launch config not yet started
              */
    STARTING,
    RUNNING,
    EXITED,
    FAILURE,
} status_t;

typedef struct wapp_state_t {
    char name[WAPP_MAX_NAME_LEN];
    char image[WAPP_MAX_NAME_LEN]; /* registry image the instance was launched
                                      from */
    uint8_t id;
    char version[WAPP_MAX_VERSION_LEN]; /* image version tag */
    status_t status;
    /* WASI exit code, or WAPP_EXIT_CODE_NONE when the wapp trapped or has not
     * exited. Authoritative only when status == EXITED. */
    int32_t exit_code;
    /* WASM linear-memory accounting for a running instance, sampled by the
     * platform from the live WAMR instance (zero for a slot with no
     * instance). bytes = pages × the module's page size. */
    uint32_t mem_pages_cur; /* linear-memory pages currently committed */
    uint32_t mem_pages_max; /* linear-memory page ceiling (per-wapp cap) */
    size_t mem_bytes_cur;   /* mem_pages_cur in bytes */
    size_t mem_bytes_max;   /* mem_pages_max in bytes */
} wapp_state_t;

/* Sample a running instance's linear-memory accounting into *out's mem_* fields
 * (zeroed with no live instance). WAMR types live in the engine core, so the
 * platform calls this rather than dereferencing its opaque wamrData_t. */
void WantedWappMemStats(const struct wamrData_t *wamr, wapp_state_t *out);

typedef struct reg_entry_t {
    char name[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
    size_t size;
} reg_entry_t;

typedef struct wantedConfig_t {
    wapp_config_t supervisorCfg;
    char supervisorImagePath
        [CONFIG_WANTED_MAX_PATH_LEN]; /* empty = use compiled-in default */
    bool privileged; /* enables privileged /proc entries when true */
} wantedConfig_t;

/**
 * Load and run a wapp to completion on the calling worker thread. Blocks until
 * it exits or traps, writing the WASI exit code to @p ctx->exit_code; teardown
 * is the caller's, through WantedWappStop().
 *
 * @param ctx  Wapp slot to run (image, VFS, and WAMR state). Must be non-NULL.
 * @return 0 on a completed run; negative on a setup failure before it ran.
 */
int WantedWappRun(wapp_data_t *ctx);

/**
 * Tear down the instance WantedWappRun() built for @p ctx, then unwind the
 * per-thread WAMR env. Idempotent on the failure path, so every worker-thread
 * exit can call it.
 *
 * @param ctx  Wapp slot previously passed to WantedWappRun().
 */
void WantedWappStop(wapp_data_t *ctx);

/**
 * Asynchronously abort a running wapp's in-flight WASM execution, so the worker
 * thread's call returns and unwinds through WantedWappStop(). The cooperative
 * stop path for platforms that cannot force thread cancellation.
 *
 * @param ctx  Wapp slot to terminate; a NULL ctx or dead instance is a no-op.
 */
void WantedWappTerminate(wapp_data_t *ctx);

/**
 * Return the process-wide supervisor wapp descriptor, built on first call from
 * the operator config or the compiled-in default. A reload armed beforehand
 * re-reads the image from the configured path first.
 *
 * @return The cached descriptor, or NULL if the config cannot be parsed.
 */
wapp_t *WantedGetCurrentSupervisor(void);

/**
 * Arm a supervisor image reload, applied at the next respawn; nothing is
 * stopped and child wapps keep running across the swap. Stage the new image by
 * atomic rename, since the engine holds the current one mapped.
 *
 * @return 0.
 */
int WantedSupervisorReload(void);

/**
 * Fall back to the compiled-in supervisor image after a staged one fails,
 * pinning it for the rest of the run and arming a reload.
 *
 * @return 0 when a fallback is available, -1 when the built-in image already
 *         runs and the caller must report the failure itself.
 */
int WantedSupervisorRollback(void);

/* Consecutive bad starts before the engine falls back to the compiled-in
 * supervisor image. */
#define MAX_SUPERVISOR_LAUNCH_FAILURES 3

/** What a platform run loop should do after one supervisor observation. */
typedef enum {
    SUPERVISOR_HEALTHY,       /**< Running. Nothing to do. */
    SUPERVISOR_RESPAWN,       /**< Gone. Start it again. */
    SUPERVISOR_ROLLED_BACK,   /**< Bad image; the compiled-in one is pinned.
                                   Start it and report the fallback. */
    SUPERVISOR_UNRECOVERABLE, /**< The compiled-in image fails too, thus the
                                   configuration is broken. Report and stop. */
} supervisorHealth_t;

/**
 * Judge supervisor health once per run-loop iteration and count bad starts
 * toward the rollback ceiling, so a platform run loop keeps only the respawn.
 * An exit is judged by how long the supervisor ran first.
 *
 * @param running  a supervisor slot is RUNNING.
 * @param failed   a supervisor slot is in FAILURE.
 * @param exited   a supervisor slot has EXITED.
 * @return what the caller should do next.
 */
supervisorHealth_t WantedSupervisorObserve(bool running, bool failed,
                                           bool exited);
