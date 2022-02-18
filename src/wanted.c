#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

#include <tiny-json.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <wanted_malloc.h>

#include <wanted.h>
#include <romfs.h>
#include <vfs.h>
#include <vfs-drivers.h>

#include <platform.h>

struct m3Data_t {
    IM3Runtime rt;
    IM3Environment env;
    m3_wasi_context_t *wasiCtx;
};

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

    ret = RomfsMapFile(r, (void **)&wasm->img, &wasm->img_len, fd, 0);

_exit:
    RomfsClose(r, fd);
    RomfsUnload(&r);

    return ret;
}

int RunWapp(data_t *ctx)
{
    M3Result status;
    IM3Module mod;
    IM3Function f;
    wapp_t wasm;
    int ret;

    ret = LoadWasmFromRomfs("app.wasm", ctx->wapp->img, ctx->wapp->img_len, &wasm);
    if (ret < 0) {
        printf("Can't load from romfs: %d\n", ret);
        return -1;
    }

    ctx->m3 = (struct m3Data_t *)WantedMalloc(sizeof(struct m3Data_t));
    if (!ctx->m3) {
        printf("Can't allocate data for m3\n");
        return -1;
    }
    memset(ctx->m3, 0, sizeof(struct m3Data_t));

    ctx->m3->env = m3_NewEnvironment();
    if (!ctx->m3->env) {
        printf("Can't allocate data for m3 env\n");
        return -1;
    }
    ctx->m3->rt =  m3_NewRuntime(ctx->m3->env, 4096, NULL);
    if (!ctx->m3->rt) {
        printf("Can't allocate data for m3 rt\n");
        return -1;
    }


    printf("entering thread: %d\n", ctx->id);
    printf("parsing wasm: %p (%ld)\n", wasm.img, wasm.img_len);
    status = m3_ParseModule(ctx->m3->env, &mod, wasm.img, wasm.img_len);
    if (status) printf("m3_ParseModule[%d]: %s", ctx->id, status);

    printf("loading wasm\n");
    status = m3_LoadModule(ctx->m3->rt, mod);
    if (status) printf("m3_LoadModule[%d]: %s", ctx->id, status);

    printf("getting context\n");
    ctx->m3->wasiCtx = InitWasiContext();
    if (!ctx->m3->wasiCtx) {
        printf("InitWasiContext: can't allocate\n");
        goto _freeM3;
    }

    ctx->vfs.main = VfsInit();
    if (!ctx->vfs.main) {
        printf("VfsInit: can't allocate\n");
        goto _freeCtx;
    }

    //    ctx->m3->is = ctx->m3->rt->stack;
    //printf("%p ?= %p\n", ctx->m3->rt->stack, ctx->m3->is);


    ctx->m3->wasiCtx->argc = 0;
    ctx->m3->wasiCtx->argv = NULL;
    ctx->m3->wasiCtx->vfsCtx = ctx->vfs.main;

    LinkWASI(mod, ctx->m3->wasiCtx);
    LinkWantedApi(mod);
    m3_LinkLibC(mod);

    ret = VfsVirtualInit(&ctx->vfs.drivers[0]);
    if (ret < 0) {
        printf("VfsVirtualInit: can't load virt driver (%d)\n", ret);
        goto _freeVfs;
    }

    ret = VfsRomfsInit(&ctx->vfs.drivers[1], "", ctx->wapp->img, ctx->wapp->img_len);
    if (ret < 0) {
        printf("VfsRomfsInit: can't load romfs (%d)\n", ret);
        goto _freeVfs;
    }

    ret = VfsPlatformInit(&ctx->vfs.drivers[2]);
    if (ret < 0) {
        printf("VfsPlatformInit: can't load platform driver (%d)\n", ret);
        goto _freeVfs;
    }

    VfsRegister(ctx->vfs.main, "<stdin>", &ctx->vfs.drivers[2]);
    VfsRegister(ctx->vfs.main, "<stdout>", &ctx->vfs.drivers[2]);
    VfsRegister(ctx->vfs.main, "<stderr>", &ctx->vfs.drivers[2]);
    VfsRegister(ctx->vfs.main, "/", &ctx->vfs.drivers[0]);
    VfsRegister(ctx->vfs.main, "/rom", &ctx->vfs.drivers[1]);
    VfsRegister(ctx->vfs.main, "/data", &ctx->vfs.drivers[2]);

    status = m3_FindFunction (&f, ctx->m3->rt, "entry");
    if (status) {
        status = m3_FindFunction (&f, ctx->m3->rt, "_start");
        if (status) {
            printf("m3_FindFunction[%d]: %s\n", ctx->id, status);
            goto _freeVfs;
        }
    }

    printf("starting wapp: %d\n", ctx->id);
    status = m3_CallV (f, (int32_t)ctx->id);
    if (status) {
        M3ErrorInfo info;
        m3_GetErrorInfo(ctx->m3->rt, &info);
        printf("m3_CallV[%d]: %s - %s\n", ctx->id, status, info.message);
    }

    printf("Normal exit\n");

_freeVfs:
    VfsPlatformDestroy(&ctx->vfs.drivers[2]);
    VfsRomfsDestroy(&ctx->vfs.drivers[1]);
    VfsVirtualDestroy(&ctx->vfs.drivers[0]);
    VfsDestroy(&ctx->vfs.main);
_freeCtx:
    FreeWasiContext(ctx->m3->wasiCtx);
_freeM3:
    m3_FreeRuntime(ctx->m3->rt);
    m3_FreeEnvironment(ctx->m3->env);
    WantedFree(ctx->m3);

    printf("RunWapp end\n");

    return 0;
}

void StopWapp(data_t *ctx)
{
    printf("StopWapp end\n");

    //printf("%p ?= %p\n", ctx->m3->rt->stack, ctx->m3->is);
    //ctx->m3->rt->stack = ctx->m3->is;

    VfsPlatformDestroy(&ctx->vfs.drivers[2]);
    printf("1\n");
    VfsRomfsDestroy(&ctx->vfs.drivers[1]);
    printf("2\n");
    VfsVirtualDestroy(&ctx->vfs.drivers[0]);
    printf("3\n");
    VfsDestroy(&ctx->vfs.main);
    printf("4\n");
    FreeWasiContext(ctx->m3->wasiCtx);
    printf("5\n");
    m3_FreeRuntime(ctx->m3->rt);
    printf("6\n");
    m3_FreeEnvironment(ctx->m3->env);
    printf("7\n");
    WantedFree(ctx->m3);
    printf("8\n");
}
