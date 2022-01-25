#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

#include <tiny-json.h>

#include <my_api.h>
#include <wasi.h>

#include <wanted.h>

#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

#ifdef WANTED_ROMFS
#include <romfs.h>

static int LoadWasmFromRomfs(const char* wasmName, uint8_t *img, size_t imgLen, wapp_t *wasm)
{
    int ret;

    if (wasmName == NULL || img == NULL || wasm == NULL) FATAL("invalid paramter");

    ret = RomfsLoad(img, imgLen);
    if (ret < 0) FATAL("load returned %d", ret);

    ret = RomfsFdStatAt(3, wasmName, NULL);
    if (ret < 0) FATAL("stat returned %d", ret);
    if (!IS_FILE(ret)) FATAL("%s is not correct file", wasmName);

    ret = RomfsOpenAt(3, wasmName, 0);
    if (ret < 0) FATAL("open returned %d", ret);

    ret = RomfsMapFile((void **)&wasm->img, &wasm->img_len, ret, 0);

    RomfsClose(ret);

    return 0;
}

#endif

int RunWapp(data_t *ctx)
{
    M3Result status;
    IM3Module mod;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 4096, NULL);
    IM3Function f;
    m3_wasi_context_t *wasiCtx;
    wapp_t wasm;

#ifdef WANTED_ROMFS
    if (0 > LoadWasmFromRomfs("app.wasm", ctx->wapp->img, ctx->wapp->img_len, &wasm)) FATAL("Can't load from romfs");
#else
    wasm.img = ctx->wapp->img;
    wasm.img_len = ctx->wapp->img_len;
#endif

    printf("entering thread: %d\n", ctx->id);
    printf("parsing wasm: %p (%ld)\n", wasm.img, wasm.img_len);
    status = m3_ParseModule(env, &mod, wasm.img, wasm.img_len);
    if (status) FATAL("m3_ParseModule[%d]: %s", ctx->id, status);

    printf("loading wasm\n");
    status = m3_LoadModule(rt, mod);
    if (status) FATAL("m3_LoadModule[%d]: %s", ctx->id, status);

    printf("getting context\n");
    wasiCtx = GetWasiContext();

    wasiCtx->argc = 0;
    wasiCtx->argv = NULL;

#ifdef WANTED_ROMFS
    wasiCtx->RomfsImg = ctx->wapp->img;
    wasiCtx->RomfsImgLen = ctx->wapp->img_len;

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

/*

    */
