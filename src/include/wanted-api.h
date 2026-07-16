/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted-config.h>
#include <wanted.h>

#define WAPP_MAX_NAME_LEN 15
#define WAPP_MAX_VERSION_LEN 15
#define MAX_DRIVER_NAME 15
/* MAX_DRIVERS_CNT and MAX_OPTIONS_SIZE are profile-tunable footprint knobs and
 * live with the other resource limits in wanted-config.h (included above). */

/* An image reference is "<name>:<tag>" — an image name plus an optional version
 * tag. It bounds the config `image` field, which may carry a pinned tag. */
#define WAPP_MAX_IMAGE_REF_LEN (WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN)

/* Command-line arguments and environment variables passed to a wapp via its
 * launch config. argv[0] is the wapp name (set by the engine at launch), so
 * these arrays hold argv[1..] and the POSIX "KEY=VALUE" environ entries. */
#define WAPP_MAX_ARGS 8
#define WAPP_MAX_ARG_LEN 64
#define WAPP_MAX_ENVS 8
#define WAPP_MAX_ENV_LEN 64

/* Sentinel exit code for a wapp that has no WASI exit code: a wapp that traps
 * never reaches proc_exit, and a wapp that has not exited yet has no code. The
 * exit_code field is authoritative only when status == EXITED. */
#define WAPP_EXIT_CODE_NONE (-1)

struct wamrData_t;

/* A launch-config resource entry. The three launch-config sections share this
 * shape but use it differently:
 *   - drivers[] — device singletons; `name` only, mounted at "/dev/<name>".
 *   - mounts[]  — file/backend drivers bound at an arbitrary absolute `path`.
 *   - sockets[] — connections at "/net/<name>"; the transport spec is carried
 *                 in `options` (the JSON "address" field).
 * Where a section forbids a field, a value present there is rejected at install
 * time. */
typedef struct wapp_driver_t {
    char name[WAPP_MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char options[MAX_OPTIONS_SIZE];
} wapp_driver_t;

typedef struct wapp_config_t {
    bool valid;
    /* Registry image this instance runs, as an image reference "<name>:<tag>"
     * (the tag is optional → first-match). Empty means "same as the instance
     * name" — preserving single-instance wapps that never set it. Set from the
     * launch config's "image" field; it lets N instances share one image. */
    char image[WAPP_MAX_IMAGE_REF_LEN];
    wapp_driver_t console[3];
    /* Device singletons, mounted at "/dev/<name>". */
    size_t driversCnt;
    wapp_driver_t drivers[MAX_DRIVERS_CNT];
    /* File/backend drivers bound at an arbitrary absolute path. */
    size_t mountsCnt;
    wapp_driver_t mounts[MAX_DRIVERS_CNT];
    /* Named connections created at "/net/<name>". */
    size_t socketsCnt;
    wapp_driver_t sockets[MAX_DRIVERS_CNT];
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
    /* WASI exit code captured from the run, or WAPP_EXIT_CODE_NONE if the wapp
     * trapped (never reached proc_exit). Authoritative only when the slot's
     * status is EXITED. Embedded in each platform's persistent wapp slot, so it
     * survives the run and feeds PlatformWappGetState. */
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

/* Sample a running WAMR instance's linear-memory accounting into *out's mem_*
 * fields (zeroed when the slot has no live instance); other fields are left
 * untouched. The platform owns the wapp slot and its opaque wamrData_t pointer,
 * but the WAMR runtime types live in the engine core — so the platform calls
 * this from PlatformWappGetState rather than dereferencing wamrData_t. */
void WantedWappMemStats(const struct wamrData_t *wamr, wapp_state_t *out);

typedef struct reg_entry_t {
    char name[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
    size_t size;
} reg_entry_t;

typedef struct wantedConfig_t {
    wapp_config_t supervisorCfg;
    char
        supervisorImagePath[MAX_PATH_LEN]; /* empty = use compiled-in default */
    bool privileged; /* enables privileged /proc entries when true */
} wantedConfig_t;

/**
 * Load and run a wapp to completion on the calling worker thread.
 *
 * Instantiates the WAMR module from the wapp's TarFS layer stack, builds its
 * WASI context and VFS from the launch config, and invokes the entry point.
 * Blocks until the wapp exits or traps. The WASI exit code (or
 * WAPP_EXIT_CODE_NONE on a trap) is written to @p ctx->exit_code; instance
 * teardown is the caller's responsibility via WantedWappStop().
 *
 * @param ctx  Wapp slot to run (image, VFS, and WAMR state). Must be non-NULL.
 * @return 0 on a completed run; negative on a setup failure (NULL ctx, WAMR
 *         init, per-thread env, load, or instantiation) before the wapp ran.
 */
int WantedWappRun(wapp_data_t *ctx);

/**
 * Tear down the instance WantedWappRun() built for @p ctx.
 *
 * Destroys the VFS, WASI context, WAMR exec-env/instance/module, and image
 * bytes, then unwinds the per-thread WAMR env. Idempotent on the failure path
 * (a failed run has already unwound its instance), so it is safe to call on
 * every worker-thread exit.
 *
 * @param ctx  Wapp slot previously passed to WantedWappRun().
 */
void WantedWappStop(wapp_data_t *ctx);

/**
 * Asynchronously abort a running wapp's in-flight WASM execution.
 *
 * Signals the WAMR instance so the worker thread's call returns and unwinds
 * through WantedWappStop(). This is the cooperative stop path for platforms
 * that cannot force thread cancellation. Self-guards when the wapp has no live
 * instance.
 *
 * @param ctx  Wapp slot to terminate; a NULL ctx or dead instance is a no-op.
 */
void WantedWappTerminate(wapp_data_t *ctx);

/**
 * Return the process-wide supervisor wapp descriptor.
 *
 * Lazily constructs the descriptor on first call from the operator config, or
 * the compiled-in default config when none is valid; the supervisor always
 * runs the factory image (a downloaded newer version is not adopted).
 *
 * @return The cached supervisor descriptor, or NULL if the config cannot be
 *         parsed.
 */
wapp_t *WantedGetCurrentSupervisor(void);
