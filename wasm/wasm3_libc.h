#pragma once

#include <stdint.h>

#define WASM_IMPORT(MODULE,NAME) __attribute__((import_module(MODULE))) __attribute__((import_name(NAME)))

WASM_IMPORT("env", "_debug")     uint32_t e_debug(char *, size_t);
WASM_IMPORT("env", "_memset")    void *e_memset(void *, int32_t, size_t);
WASM_IMPORT("env", "_memmove")   void *e_memmove(void *, const void *, unsigned long);
WASM_IMPORT("env", "_memcpy")    void *e_memcpy(void *, const void *, unsigned long);
WASM_IMPORT("env", "_abort")     void e_abort();
WASM_IMPORT("env", "_exit")      void e_exit(int32_t);
WASM_IMPORT("env", "clock_ms")   uint32_t e_clock_ms();
WASM_IMPORT("env", "printf")     int32_t e_print(const char *, ...);

WASM_IMPORT("wanted", "sleep")        void w_sleep(uint32_t);

WASM_IMPORT("wanted", "send")        void w_send(const char *, size_t);
WASM_IMPORT("wanted", "recv")        void w_recv(char *, size_t);

WASM_IMPORT("wanted", "get_rand")    unsigned w_get_rand();
