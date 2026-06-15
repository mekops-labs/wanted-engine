/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted.h>

// TODO: make configurable
#define MAX_WAPPS 3
#define WAPP_MAX_NAME_LEN 15
#define WAPP_MAX_VERSION_LEN 15
#define MAX_DRIVER_NAME 15
#define MAX_PATH_LEN 256
#define MAX_OPTIONS_SIZE 1024
#define MAX_DRIVERS_CNT 10

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
} wapp_state_t;

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

int WantedWappRun(wapp_data_t *ctx);
void WantedWappStop(wapp_data_t *ctx);
void WantedWappTerminate(wapp_data_t *ctx);
wapp_t *WantedGetCurrentSupervisor(void);
