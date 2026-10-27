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

/* One ring per wapp the engine can run, plus one for the engine's own
 * error channel, so a full table's LRU eviction cannot drop the
 * engine's own record of a failure. */
#define CONFIG_WANTED_LOG_SLOTS (CONFIG_WANTED_MAX_WAPPS + 1)

/* Version of the contract between the engine and a supervisor wapp: the shape
 * of the /dev/wanted control plane, its verbs, and the wapp states it reports.
 * Bump only when a supervisor built against an earlier value would misread. */
#define WANTED_SUPERVISOR_ABI 2

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
    /* Reads the privileged /proc entries (`wapps`, `memory`). */
    bool privileged;
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
    /* Run the firmware's own supervisor when it is newer than the installed
     * one a versionless registry reference resolves to. Cleared by
     * `.supervisor.keepInstalled`, which holds the installed one. */
    bool supervisorPreferBundled;
} wantedConfig_t;

/** Run a non-NULL @p ctx to completion on this worker thread, writing its WASI
 *  exit code to @p ctx->exit_code; WantedWappStop() tears it down. Returns 0
 *  when it ran, negative on a setup failure. */
int WantedWappRun(wapp_data_t *ctx);

/** Tear down what WantedWappRun() built for @p ctx and unwind the per-thread
 *  WAMR env. Idempotent, so every worker-thread exit may call it. */
void WantedWappStop(wapp_data_t *ctx);

/** Abort @p ctx's in-flight WASM so its worker unwinds through
 *  WantedWappStop(), for platforms without thread cancellation. A NULL ctx or
 *  a dead instance is a no-op. */
void WantedWappTerminate(wapp_data_t *ctx);

/** The supervisor descriptor, built on first call from the config or the
 *  compiled-in default, and re-read first when a reload is armed. NULL when
 *  the config cannot be parsed. */
wapp_t *WantedGetCurrentSupervisor(void);

/** Arm a supervisor reload for the next respawn; child wapps keep running.
 *  Stage the new image by atomic rename, since the current one stays mapped.
 *  Returns 0. */
int WantedSupervisorReload(void);

/** Pin the compiled-in supervisor image after a staged one fails and arm a
 *  reload. Returns 0, or -1 when the built-in image already runs. */
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

/** Judge supervisor health once per run-loop pass, counting bad starts toward
 *  the rollback ceiling; an exit is judged by how long it ran. Each flag says
 *  a supervisor slot is RUNNING, in FAILURE, or EXITED. */
supervisorHealth_t WantedSupervisorObserve(bool running, bool failed,
                                           bool exited);
