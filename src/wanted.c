#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

#include <tiny-json.h>

#include "my_api.h"
#include "wasi/wasi.h"

#include <wanted.h>

#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

int RunWapp(data_t *ctx)
{
    M3Result status;
    IM3Module mod;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 4096, NULL);
    IM3Function f;
    m3_wasi_context_t *wasiCtx;

    printf("entering thread: %d\n", ctx->id);

    printf("parsing wasm: %p (%ld)\n",ctx->wapp->wasm, ctx->wapp->wasm_len);
    status = m3_ParseModule(env, &mod, ctx->wapp->wasm, ctx->wapp->wasm_len);
    if (status) FATAL("m3_ParseModule[%d]: %s", ctx->id, status);

    printf("loading wasm\n");
    status = m3_LoadModule(rt, mod);
    if (status) FATAL("m3_LoadModule[%d]: %s", ctx->id, status);

    printf("getting context\n");
    wasiCtx = GetWasiContext();

    wasiCtx->argc = 0;
    wasiCtx->argv = NULL;

#ifdef WANTED_ROMFS
    wasiCtx->RomfsImg = ctx->romfs.img;
    wasiCtx->RomfsImgLen = ctx->romfs.len;

    printf("romfs: %p (%ld)\n", wasiCtx->RomfsImg, wasiCtx->RomfsImgLen);
#endif

    LinkWASI(mod);
    LinkMyApi(mod);
    m3_LinkLibC(mod);

    status = m3_FindFunction (&f, rt, "entry");
    if (status) {
        status = m3_FindFunction (&f, rt, "_start");
        if (status) FATAL("m3_FindFunction[%d]: %s", ctx->id, status);
    }

    //printf("starting wapp: %d\n", ctx->id);
    status = m3_CallV (f, (int32_t)ctx->id);
    if (status) {
        M3ErrorInfo info;
        m3_GetErrorInfo(rt, &info);
        FATAL("m3_CallV[%d]: %s - %s", ctx->id, status, info.message);
    }

    return 0;
}
