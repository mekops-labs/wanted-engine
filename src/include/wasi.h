#pragma once

#include <stdint.h>
#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct m3_wasi_context_t {
    int32_t     exit_code;
    uint32_t    argc;
    const char **argv;
    vfs_ctx_t   vfsCtx;
} m3_wasi_context_t;

void RegisterWASINatives(void);

m3_wasi_context_t *InitWasiContext(void);
void FreeWasiContext(m3_wasi_context_t *);

#ifdef __cplusplus
}
#endif
