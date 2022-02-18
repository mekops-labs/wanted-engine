#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vfs.h>

typedef struct m3Data_t *im3Data_t;

typedef struct {
    uint8_t *img;
    size_t img_len;
} wapp_t;

typedef struct {
    vfs_ctx_t main;
    vfs_driver_t drivers[10];
} vfs_ctxs_t;

typedef struct {
    uint8_t id;
    wapp_t *wapp;
    vfs_ctxs_t vfs;
    im3Data_t m3;
} data_t;

int  RunWapp(data_t *ctx);
void StopWapp(data_t *ctx);
