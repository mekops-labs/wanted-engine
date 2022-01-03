#pragma once

#include <stdint.h>

#define WASM_IMPORT(MODULE,NAME) __attribute__((import_module(MODULE))) __attribute__((import_name(NAME)))

WASM_IMPORT("env", "_debug")     uint32_t debug(char *, size_t);
WASM_IMPORT("env", "_memset")    void *memset(void *, int32_t, size_t);
WASM_IMPORT("env", "_memmove")   void *memmove(void *, const void *, unsigned long);
WASM_IMPORT("env", "_memcpy")    void *memcpy(void *, const void *, unsigned long);
WASM_IMPORT("env", "_abort")     void abort();
WASM_IMPORT("env", "_exit")      void exit(int32_t);
WASM_IMPORT("env", "clock_ms")   uint32_t clock_ms();
WASM_IMPORT("env", "printf")     int32_t print(const char *, ...);
