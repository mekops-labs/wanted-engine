#pragma once

#include <stdint.h>
#include <stddef.h>

#ifndef WANTED_CUSTOM_MALLOC
#   include <stdlib.h>
#   define WantedMalloc(x)  malloc(x)
#   define WantedFree(x)  free(x)
#else
void *WantedMalloc(size_t s);
void WantedFree(void* ptr);
#endif

typedef struct {
    uint8_t *img;
    size_t img_len;
} wapp_t;

typedef struct {
    uint8_t id;
    wapp_t *wapp;
} data_t;

int RunWapp(data_t *ctx);
void MyApiInit(void);
