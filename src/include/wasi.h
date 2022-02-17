#pragma once

#include "m3_core.h"
#include <vfs.h>
#include <romfs.h>

d_m3BeginExternC

typedef struct m3_wasi_context_t
{
    i32                     exit_code;
    u32                     argc;
    ccstr_t *               argv;
    vfs_ctx_t               vfsCtx;
} m3_wasi_context_t;

    M3Result    LinkWASI     (IM3Module io_module, m3_wasi_context_t* ctx);

m3_wasi_context_t* InitWasiContext();
void FreeWasiContext(m3_wasi_context_t *);

d_m3EndExternC
