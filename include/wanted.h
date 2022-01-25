#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t *wasm;
    size_t wasm_len;
} wapp_t;

#ifdef WANTED_ROMFS
typedef struct
{
    uint8_t *img;
    size_t len;
} romfsimg_t;
#endif

typedef struct {
    uint8_t id;
    wapp_t *wapp;
#ifdef WANTED_ROMFS
    romfsimg_t romfs;
#endif
} data_t;



int RunWapp(data_t *ctx);
void MyApiInit(void);
