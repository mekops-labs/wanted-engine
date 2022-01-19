#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <pthread.h>

#include <sys/mman.h>

#include <wasm3.h>
#include <m3_api_libc.h>

#include <tiny-json.h>

#include "my_api.h"
#include "wasi/wasi.h"

#ifdef WANTED_ROMFS
#include <romfs.h>
#endif

#define FATAL_N(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return NULL; }
#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

typedef struct {
    uint8_t *wasm;
    size_t wasm_len;
} wapp_t;

#ifdef WANTED_ROMFS
typedef struct
{
    uint8_t *img;
    size_t len;
} romfsimg_t;

romfsimg_t romfs;
#endif

typedef struct {
    uint8_t id;
    wapp_t *wapp;
} data_t;

void *WA_thread( void *ptr )
{
    data_t *ctx = ptr;
    M3Result status;
    IM3Module mod;
    IM3Environment env = m3_NewEnvironment();
    IM3Runtime rt = m3_NewRuntime(env, 4096, NULL);
    IM3Function f;
    m3_wasi_context_t *wasiCtx;

    printf("entering thread: %d\n", ctx->id);

    printf("parsing wasm: %p (%ld)\n",ctx->wapp->wasm, ctx->wapp->wasm_len);
    status = m3_ParseModule(env, &mod, ctx->wapp->wasm, ctx->wapp->wasm_len);
    if (status) FATAL_N("m3_ParseModule[%d]: %s", ctx->id, status);

    printf("loading wasm\n");
    status = m3_LoadModule(rt, mod);
    if (status) FATAL_N("m3_LoadModule[%d]: %s", ctx->id, status);

    printf("getting context\n");
    wasiCtx = GetWasiContext();

    wasiCtx->argc = 0;
    wasiCtx->argv = NULL;

#ifdef WANTED_ROMFS
    wasiCtx->RomfsImg = romfs.img;
    wasiCtx->RomfsImgLen = romfs.len;

    printf("romfs: %p (%ld)\n", wasiCtx->RomfsImg, wasiCtx->RomfsImgLen);
#endif

    LinkWASI(mod);
    LinkMyApi(mod);
    m3_LinkLibC(mod);

    status = m3_FindFunction (&f, rt, "entry");
    if (status) {
        status = m3_FindFunction (&f, rt, "_start");
        if (status) FATAL_N("m3_FindFunction[%d]: %s", ctx->id, status);
    }

    //printf("starting wapp: %d\n", ctx->id);
    status = m3_CallV (f, (int32_t)ctx->id);
    if (status) {
        M3ErrorInfo info;
        m3_GetErrorInfo(rt, &info);
        FATAL_N("m3_CallV[%d]: %s - %s", ctx->id, status, info.message);
    }

    return (void *)status;
}

#define MAX_WAPPS 3

typedef struct {
    pthread_t t;
    data_t data;
} thread_data_t;

struct {
    wapp_t wapps[MAX_WAPPS];
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} config;


int loadWapp(const char *filename, wapp_t * wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;

    f = fopen(filename, "rb");
    if (NULL == f) FATAL("can't open %s", filename);

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED) FATAL("can't map file");

#ifndef WANTED_ROMFS
    wapp->wasm = img;
    wapp->wasm_len = filesize;
#else
    int ret;
    char *wasmName = "app.wasm";

    romfs.img = img;
    romfs.len = filesize;

    ret = RomfsLoad(img, filesize);
    if (ret < 0) FATAL("load returned %d", ret);

    ret = RomfsFdStatAt(3, wasmName, NULL);
    if (ret < 0) FATAL("stat returned %d", ret);
    if (!IS_FILE(ret)) FATAL("%s is not correct file", wasmName);

    ret = RomfsOpenAt(3, wasmName, 0);
    if (ret < 0) FATAL("open returned %d", ret);

    ret = RomfsMapFile((void **)&wapp->wasm, &wapp->wasm_len, ret, 0);

    RomfsClose(ret);
#endif

    fclose(f);
    return 0;
}

int main(int argc, char* argv[]) {
    wapp_t wapp;

    if (argc < 2) FATAL("Need at least 1 argument");

    for (int i = 0; i < argc - 1; i++) {
        if (loadWapp(argv[i+1], &config.wapps[i]) < 0) FATAL("Wapp loading failed");
    }
    config.n = argc - 1;

    MyApiInit();

    for (int i = 0; i < config.n; i++) {
        config.threads[i].data.id   = i;
        config.threads[i].data.wapp = &config.wapps[i];

        pthread_create( &config.threads[i].t, NULL, WA_thread, (void*) &config.threads[i].data);
    }

    for (int i = 0; i < config.n; i++) {
        pthread_join( config.threads[i].t, NULL);
    }

    return 0;
}
