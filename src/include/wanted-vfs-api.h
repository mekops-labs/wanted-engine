#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <wanted.h>


int WantedSetConfig(wantedConfig_t cfg);
int WantedGetConfig(uint8_t *buf, size_t bufLen);

int WantedReadRegistry(uint8_t *buf, size_t bufLen);
int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen);
int WantedCloseRegistry();
int WantedRegistryRemove(const char *name);

int WantedReadState(uint8_t *buf, size_t bufLen);
