#pragma once

#include "m3_core.h"

d_m3BeginExternC

typedef struct m3_wasi_context_t
{
    i32                     exit_code;
    u32                     argc;
    ccstr_t *               argv;
#ifdef WANTED_ROMFS
    uint8_t *RomfsImg;
    size_t RomfsImgLen;
#endif
} m3_wasi_context_t;

    M3Result    LinkWASI     (IM3Module io_module);

m3_wasi_context_t* GetWasiContext();

d_m3EndExternC
