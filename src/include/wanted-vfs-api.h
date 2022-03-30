#pragma once

#include <stdint.h>
#include <stddef.h>
#include <wanted.h>


int WantedSetConfig(wantedConfig_t cfg);
int WantedGetConfig(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedReadState(uint8_t *buf, size_t bufLen);
