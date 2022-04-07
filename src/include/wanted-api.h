#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vfs.h>
#include <wanted.h>

typedef struct m3Data_t *im3Data_t;

typedef struct {
    union {
        struct {
            uint8_t major;
            uint8_t minor;
            uint8_t patch;
        };
        uint8_t v[3];
    };
} wapp_version_t;

typedef struct {
    char name[WAPP_MAX_NAME_LEN];
    wapp_version_t version;
    uint8_t *img;
    size_t img_len;
} wapp_t;

typedef struct {
    uint8_t id;
    wapp_t wapp;
    vfs_ctx_t vfs;
    im3Data_t m3;
    int lastStatus;
} wapp_data_t;

typedef enum {
    NOT_STARTED,
    STARTING,
    RUNNING,
    EXITED,
    FAILURE,
} status_t;

typedef struct {
    char name[WAPP_MAX_NAME_LEN];
    uint8_t id;
    wapp_version_t version;
    status_t status;
} wapp_state_t;

int  WantedWappRun(wapp_data_t *ctx);
void WantedWappStop(wapp_data_t *ctx);
int  WantedWappParseManifest(wapp_t *w);
wapp_t WantedGetCurrentSupervisor();

