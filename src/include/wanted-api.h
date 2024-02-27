#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

typedef struct m3Data_t *im3Data_t;

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
} wapp_config_t;

typedef enum wapp_action_t {
    WAPP_START,
    WAPP_STOP,
} wapp_action_t;

typedef struct wapp_t {
    char name[WAPP_MAX_NAME_LEN];
    wapp_version_t version;
    uint8_t *img;
    size_t img_len;
    wapp_config_t cfg;
} wapp_t;

typedef struct wapp_data_t {
    uint8_t id;
    wapp_t *wapp;
    vfs_ctx_t vfs;
    im3Data_t m3;
    int lastStatus;
} wapp_data_t;

typedef enum status_t {
    NOT_STARTED,
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
} wapp_state_t;

typedef struct reg_entry_t {
    char name[WAPP_MAX_NAME_LEN];
    char version[WAPP_MAX_VERSION_LEN];
    size_t size;
} reg_entry_t;

// TODO: this is somewhat too simple, make it more dynamic

typedef struct wantedConfig_t {
    char wappsToRun[MAX_WAPPS][WAPP_MAX_NAME_LEN];
    int nWapps;
    wapp_config_t supervisorCfg;
} wantedConfig_t;

int WantedWappRun(wapp_data_t *ctx);
void WantedWappStop(wapp_data_t *ctx);
int WantedWappParseManifest(wapp_t *w);
int WantedWappLoadManifest(const wapp_t *w, uint8_t **img, size_t *imgLen);
wapp_t *WantedGetCurrentSupervisor();
