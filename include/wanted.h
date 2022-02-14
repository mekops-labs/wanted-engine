#pragma once

#include <stdint.h>
#include <stddef.h>

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
