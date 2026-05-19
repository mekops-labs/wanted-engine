#pragma once

#include <stdlib.h>

// TODO: make configurable
#define WASM_STACK_SIZE 8192
#define WASM_HEAP_SIZE  8192

int WantedStart(const char *cfg, size_t cfgLen);
