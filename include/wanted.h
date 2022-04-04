#pragma once

#include <stdint.h>
#include <stddef.h>

#define MAX_WAPPS 3
#define WAPP_MAX_NAME_LEN 15

typedef struct {
    char    wappsToRun[MAX_WAPPS][WAPP_MAX_NAME_LEN];
    int     nWapps;
} wantedConfig_t;

int WantedStart(wantedConfig_t cfg);
