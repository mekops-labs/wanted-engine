#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

#include <tiny-json.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <wanted.h>
#include <romfs.h>
#include <vfs.h>
#include <vfs-drivers.h>

#include <platform.h>


static int LoadWasmFromRomfs(const char* wasmName, uint8_t *img, size_t imgLen, wapp_t *wasm)
{
    int ret, fd;
    romfs_t r;

    if (wasmName == NULL || img == NULL || wasm == NULL) {
        printf("LoadWasmFromRomfs: invalid paramter\n");
        return -1;
    }

    ret = RomfsLoad(img, imgLen, &r);
    if (ret < 0) {
        printf("LoadWasmFromRomfs: RomfsLoad returned %d\n", ret);
        return -1;
    }

    ret = RomfsFdStatAt(r, 3, wasmName, NULL);
    if (ret < 0) {
        printf("LoadWasmFromRomfs: RomfsFdStatAt returned %d\n", ret);
        goto _exit;
    } else if (!IS_FILE(ret)) {
        printf("LoadWasmFromRomfs: %s is not correct file\n", wasmName);
        goto _exit;
    }

    fd = RomfsOpenAt(r, 3, wasmName, 0);
    if (fd < 0) {
        printf("LoadWasmFromRomfs: open returned %d\n", ret);
        goto _exit;
    }

    ret = RomfsMapFile(r, (void **)&wasm->img, &wasm->img_len, ret, 0);

_exit:
    RomfsClose(r, fd);
    RomfsUnload(&r);

    return ret;
}

int RunWapp(data_t *ctx)
{
    M3Result status;
    IM3Module mod;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 4096, NULL);
    IM3Function f;
    m3_wasi_context_t *wasiCtx;
    wapp_t wasm;
    vfs_driver_t platDrv;
    vfs_driver_t rootDrv;
    vfs_driver_t romfsDrv;
    int ret;

    if (0 > LoadWasmFromRomfs("app.wasm", ctx->wapp->img, ctx->wapp->img_len, &wasm)) printf("Can't load from romfs");

    printf("entering thread: %d\n", ctx->id);
    printf("parsing wasm: %p (%ld)\n", wasm.img, wasm.img_len);
    status = m3_ParseModule(env, &mod, wasm.img, wasm.img_len);
    if (status) printf("m3_ParseModule[%d]: %s", ctx->id, status);

    printf("loading wasm\n");
    status = m3_LoadModule(rt, mod);
    if (status) printf("m3_LoadModule[%d]: %s", ctx->id, status);

    printf("getting context\n");
    wasiCtx = InitWasiContext();
    if (!wasiCtx) {
        printf("InitWasiContext: can't allocate\n");
        goto _freeM3;
    }

    vfs_ctx_t c = VfsInit();
    if (!c) {
        printf("VfsInit: can't allocate\n");
        goto _freeCtx;
    }

    wasiCtx->argc = 0;
    wasiCtx->argv = NULL;
    wasiCtx->vfsCtx = c;

    LinkWASI(mod, wasiCtx);
    LinkWantedApi(mod);
    m3_LinkLibC(mod);

    ret = VfsVirtualInit(&rootDrv);
    if (ret < 0) {
        printf("VfsVirtualInit: can't load virt driver (%d)\n", ret);
        goto _freeVfs;
    }

    ret = VfsRomfsInit(&romfsDrv, "", ctx->wapp->img, ctx->wapp->img_len);
    if (ret < 0) {
        printf("VfsRomfsInit: can't load romfs (%d)\n", ret);
        goto _freeVfs;
    }

    ret = VfsPlatformInit(&platDrv);
    if (ret < 0) {
        printf("VfsPlatformInit: can't load platform driver (%d)\n", ret);
        goto _freeVfs;
    }

    VfsRegister(c, "<stdin>", &platDrv);
    VfsRegister(c, "<stdout>", &platDrv);
    VfsRegister(c, "<stderr>", &platDrv);
    VfsRegister(c, "/", &rootDrv);
    VfsRegister(c, "/rom", &romfsDrv);
    VfsRegister(c, "/data", &platDrv);

    status = m3_FindFunction (&f, rt, "entry");
    if (status) {
        status = m3_FindFunction (&f, rt, "_start");
        if (status) {
            printf("m3_FindFunction[%d]: %s\n", ctx->id, status);
            goto _freeVfs;
        }
    }

    printf("starting wapp: %d\n", ctx->id);
    status = m3_CallV (f, (int32_t)ctx->id);
    if (status) {
        M3ErrorInfo info;
        m3_GetErrorInfo(rt, &info);
        printf("m3_CallV[%d]: %s - %s\n", ctx->id, status, info.message);
    }

_freeVfs:
    VfsRomfsDestroy(&romfsDrv);
    VfsPlatformDestroy(&platDrv);
    VfsVirtualDestroy(&rootDrv);
    VfsDestroy(&c);
_freeCtx:
    FreeWasiContext(wasiCtx);
_freeM3:
    m3_FreeRuntime(rt);
    m3_FreeEnvironment(env);

    return 0;
}
