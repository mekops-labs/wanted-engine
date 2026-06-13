/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>
#include <vfs-tarfs.h>
#include <wanted.h>

// TODO: make configurable
#define MAX_WAPPS 3
#define WAPP_MAX_NAME_LEN 15
#define WAPP_MAX_VERSION_LEN 15
#define MAX_DRIVER_NAME 15
#define MAX_PATH_LEN 256
#define MAX_OPTIONS_SIZE 1024
#define MAX_DRIVERS_CNT 10
#define WAPP_MAX_REQUIREMENTS 8
#define WAPP_MAX_REQ_NAME_LEN 32

/* Per-wapp persistent state preopens. Each entry is a host directory path
 * that the Engine will create (if absent), open, and expose to the wapp as a
 * WASI preopen at the same path. Any wapp can declare these via its launch
 * config (params.preopens). */
#define WAPP_MAX_PREOPENS 4
#define WAPP_MAX_PREOPEN_LEN 64

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

typedef struct wapp_version_t {
    union {
        struct {
            uint8_t major;
            uint8_t minor;
            uint8_t patch;
            uint8_t package;
        };
        uint8_t v[4];
    };
} wapp_version_t;

// TODO: make dynamic
typedef struct wapp_driver_t {
    char name[WAPP_MAX_NAME_LEN];
    char path[MAX_PATH_LEN];
    char options[MAX_OPTIONS_SIZE];
} wapp_driver_t;

// TODO: make dynamic driver number
typedef struct wapp_config_t {
    bool valid;
    wapp_driver_t console[3];
    size_t driversCnt;
    wapp_driver_t drivers[MAX_DRIVERS_CNT];
    char preopens[WAPP_MAX_PREOPENS][WAPP_MAX_PREOPEN_LEN];
    size_t preopensCnt;
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
    char name[WAPP_MAX_NAME_LEN];
    wapp_version_t version;
    wapp_config_t cfg;
    /* OCI layer stack (newest first, index 0 = topmost) */
    uint8_t *layers[TARFS_MAX_LAYERS];
    size_t layer_lens[TARFS_MAX_LAYERS];
    uint8_t layer_cnt; /* must be >= 1 for a valid wapp */
    char requirements[WAPP_MAX_REQUIREMENTS][WAPP_MAX_REQ_NAME_LEN];
    uint8_t requirementsCnt;
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
    CREATED, /* namespace reserved via `create`, launch config not yet started */
    STARTING,
    RUNNING,
    EXITED,
    FAILURE,
} status_t;

typedef struct wapp_state_t {
    char name[WAPP_MAX_NAME_LEN];
    uint8_t id;
    wapp_version_t version;
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
    char supervisorImagePath[MAX_PATH_LEN]; /* empty = use compiled-in default */
    bool privileged; /* enables privileged /proc entries when true */
} wantedConfig_t;

int WantedWappRun(wapp_data_t *ctx);
void WantedWappStop(wapp_data_t *ctx);
void WantedWappTerminate(wapp_data_t *ctx);
int WantedWappParseManifest(wapp_t *w);
int WantedWappParseManifestBytes(wapp_t *w, const uint8_t *manifest,
                                 size_t manifestLen);
int WantedWappLoadManifest(const wapp_t *w, uint8_t **img, size_t *imgLen);
wapp_t *WantedGetCurrentSupervisor();
