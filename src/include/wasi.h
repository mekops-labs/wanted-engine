#pragma once

#include <stdint.h>
#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wasi_ctx_t {
    int32_t     exit_code;
    uint32_t    argc;
    const char **argv;
    vfs_ctx_t   vfsCtx;
} wasi_ctx_t;

void RegisterWASINatives(void);

wasi_ctx_t *InitWasiContext(void);
void FreeWasiContext(wasi_ctx_t *);

#ifdef __cplusplus
}
#endif
