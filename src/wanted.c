#include <m3_api_libc.h>
#include <m3_env.h>
#include <wasm3.h>

#include <tiny-json.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <debug_trace.h>
#include <wanted_malloc.h>

#include <vfs-drivers.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <wanted.h>

#include <platform.h>

struct m3Data_t {
    m3_wasi_context_t *wasiCtx;
    IM3Runtime rt;
    IM3Environment env;
};

/* Build a one-shot tarfs ctx over the wapp's layer stack. Caller owns the
 * returned ctx and must TarFsDestroy() it. Returns NULL on bad args / OOM /
 * malformed layer. */
static vfs_tarfs_ctx_t *WappTarfsInit(const wapp_t *w) {
    if (!w || w->layer_cnt == 0)
        return NULL;
    return TarFsInit((uint8_t *const *)w->layers, w->layer_lens, w->layer_cnt);
}

int WantedWappLoadManifest(const wapp_t *w, uint8_t **img, size_t *imgLen) {
    if (!w || !img || !imgLen)
        return -1;

    vfs_tarfs_ctx_t *t = WappTarfsInit(w);
    if (!t) {
        DEBUG_TRACE("Can't initialize tarfs for wapp manifest lookup");
        return -1;
    }

    size_t mlen = 0;
    const uint8_t *m = TarFsEntrypointManifest(t, &mlen);
    if (!m) {
        DEBUG_TRACE("manifest.json absent from wapp image");
        TarFsDestroy(t);
        return -1;
    }

    /* Pointer is into the wapp's layer bytes, which outlive this ctx. */
    *img = (uint8_t *)m;
    *imgLen = mlen;
    TarFsDestroy(t);
    return 0;
}

int WantedWappParseManifestBytes(wapp_t *w, const uint8_t *manifest,
                                 size_t manifestLen) {
    int ret = 0;
    json_t mem[32];

    if (!w || !manifest || manifestLen == 0)
        return -1;

    char *buf = WantedMalloc(manifestLen);
    if (buf == NULL) {
        DEBUG_TRACE("Can't allocate mem for manifest json buffer");
        return -1;
    }
    memcpy(buf, manifest, manifestLen);

    json_t const *json = json_create(buf, mem, sizeof mem / sizeof *mem);
    if (!json) {
        DEBUG_TRACE("Error json create.");
        ret = -1;
        goto _exit;
    }

    json_t const *name = json_getProperty(json, "name");
    if (!name || JSON_TEXT != json_getType(name)) {
        DEBUG_TRACE("Error, the name property is not found.");
        ret = -1;
        goto _exit;
    }
    strncpy(w->name, json_getValue(name), WAPP_MAX_NAME_LEN);

    json_t const *varsionArr = json_getProperty(json, "version");
    if (!varsionArr || JSON_ARRAY != json_getType(varsionArr)) {
        DEBUG_TRACE("Error, the version property is not found.");
        ret = -1;
        goto _exit;
    }

    json_t const *version;
    int i;
    for (i = 0, version = json_getChild(varsionArr); i < 3 && version != 0;
         i++, version = json_getSibling(version)) {
        if (JSON_INTEGER == json_getType(version)) {
            w->version.v[i] = (uint8_t)json_getInteger(version);
        }
    }

    json_t const *pkg = json_getProperty(json, "package");
    if (!pkg || JSON_INTEGER != json_getType(pkg)) {
        DEBUG_TRACE("Warning, the package property is not found.");
        w->version.package = 0;
    } else {
        w->version.package = (uint8_t)json_getInteger(pkg);
    }

_exit:
    WantedFree(buf);

    return ret;
}

int WantedWappParseManifest(wapp_t *w) {
    uint8_t *manifest;
    size_t manifestLen;
    int ret = WantedWappLoadManifest(w, &manifest, &manifestLen);
    if (ret < 0) {
        DEBUG_TRACE("Can't load manifest");
        return -1;
    }
    return WantedWappParseManifestBytes(w, manifest, manifestLen);
}

int WantedWappRun(wapp_data_t *ctx) {
    M3Result status;
    IM3Module mod;
    IM3Function f;
    wapp_t *wapp;
    vfs_tarfs_ctx_t *tarfs = NULL;
    const uint8_t *manifest = NULL;
    const uint8_t *wasm = NULL;
    size_t manifestLen = 0, wasmLen = 0;
    int ret = 0;

    if (ctx == NULL) {
        DEBUG_TRACE("ctx is NULL");
        return -1;
    }

    wapp = ctx->wapp;

    DEBUG_TRACE("entering thread: %d", ctx->id);

    /* Build the per-wapp tarfs index once. The pre-fetched entrypoint
     * pointers feed both manifest parsing and wasm load below; the same ctx
     * is later attached to the vfs so the prefix router can resolve arbitrary
     * paths into the same layer stack. */
    tarfs = WappTarfsInit(wapp);
    if (!tarfs) {
        DEBUG_TRACE("Can't initialize tarfs for wapp");
        return -1;
    }

    manifest = TarFsEntrypointManifest(tarfs, &manifestLen);
    if (!manifest) {
        DEBUG_TRACE("manifest.json absent from wapp image");
        ret = -1;
        goto _freeTarfs;
    }

    if (WantedWappParseManifestBytes(wapp, manifest, manifestLen) < 0) {
        DEBUG_TRACE("Can't parse wapp manifest");
        ret = -1;
        goto _freeTarfs;
    }

    wasm = TarFsEntrypointWasm(tarfs, &wasmLen);
    if (!wasm) {
        DEBUG_TRACE("app.wasm absent from wapp image");
        ret = -1;
        goto _freeTarfs;
    }

    ctx->m3 = (struct m3Data_t *)WantedMalloc(sizeof(struct m3Data_t));
    if (!ctx->m3) {
        DEBUG_TRACE("Can't allocate data for m3");
        ret = -1;
        goto _freeTarfs;
    }
    memset(ctx->m3, 0, sizeof(struct m3Data_t));

    ctx->m3->env = m3_NewEnvironment();
    if (!ctx->m3->env) {
        DEBUG_TRACE("Can't allocate data for m3 env");
        ret = -1;
        goto _freeM3;
    }
    ctx->m3->rt = m3_NewRuntime(ctx->m3->env, M3_STACK_SIZE, NULL);
    if (!ctx->m3->rt) {
        DEBUG_TRACE("Can't allocate data for m3 rt");
        ret = -1;
        goto _freeM3;
    }

    DEBUG_TRACE("parsing wasm: %p (%zu)", wasm, wasmLen);
    status = m3_ParseModule(ctx->m3->env, &mod, wasm, wasmLen);
    if (status) {
        DEBUG_TRACE("m3_ParseModule[%d]: %s", ctx->id, status);
    }

    DEBUG_TRACE("loading wasm");
    status = m3_LoadModule(ctx->m3->rt, mod);
    if (status) {
        DEBUG_TRACE("m3_LoadModule[%d]: %s", ctx->id, status);
    }

    DEBUG_TRACE("getting context");
    ctx->m3->wasiCtx = InitWasiContext();
    if (!ctx->m3->wasiCtx) {
        DEBUG_TRACE("InitWasiContext: can't allocate");
        goto _freeM3;
    }

    ctx->vfs = VfsInit();
    if (!ctx->vfs) {
        DEBUG_TRACE("VfsInit: can't allocate");
        goto _freeCtx;
    }

    /* Hand the tarfs ctx off — VfsDestroy now frees it. */
    VfsAttachTarfs(ctx->vfs, tarfs);
    tarfs = NULL;

    ctx->m3->wasiCtx->argc = 0;
    ctx->m3->wasiCtx->argv = NULL;
    ctx->m3->wasiCtx->vfsCtx = ctx->vfs;

    LinkWASI(mod, ctx->m3->wasiCtx);
    LinkWantedApi(mod);
    m3_LinkLibC(mod);

    /* install console */
    ret = WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[0].name,
                              "<stdin>", wapp->cfg.console[0].options);
    ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[1].name,
                               "<stdout>", wapp->cfg.console[1].options);
    ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[2].name,
                               "<stderr>", wapp->cfg.console[2].options);

    /* fs drivers */
    for (int i = 0; i < wapp->cfg.driversCnt; i++) {
        ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.drivers[i].name,
                                   wapp->cfg.drivers[i].path,
                                   wapp->cfg.drivers[i].options);
    }

    if (ret < 0) {
        DEBUG_TRACE("error installing drivers");
        goto _freeVfs;
    }

    status = m3_FindFunction(&f, ctx->m3->rt, "entry");
    if (status) {
        status = m3_FindFunction(&f, ctx->m3->rt, "_start");
        if (status) {
            DEBUG_TRACE("m3_FindFunction[%d]: %s", ctx->id, status);
            goto _freeVfs;
        }
    }

    DEBUG_TRACE("starting wapp: %d", ctx->id);
    status = m3_CallV(f, (int32_t)ctx->id);
    if (status) {
        M3ErrorInfo info;
        m3_GetErrorInfo(ctx->m3->rt, &info);
        DEBUG_TRACE("m3_CallV[%d]: %s - %s", ctx->id, status, info.message);
    }

    DEBUG_TRACE("normal exit");

    return 0;

_freeVfs:
    VfsDestroy(&ctx->vfs);
_freeCtx:
    FreeWasiContext(ctx->m3->wasiCtx);
_freeM3:
    if (ctx->m3->rt)
        m3_FreeRuntime(ctx->m3->rt);
    if (ctx->m3->env)
        m3_FreeEnvironment(ctx->m3->env);
    WantedFree(ctx->m3);
_freeTarfs:
    if (tarfs)
        TarFsDestroy(tarfs);

    DEBUG_TRACE("end");

    return ret < 0 ? ret : -1;
}

void WantedWappStop(wapp_data_t *ctx) {
    if (ctx->lastStatus != 0)
        return;

    DEBUG_TRACE("start");

    VfsDestroy(&ctx->vfs);
    FreeWasiContext(ctx->m3->wasiCtx);
    m3_FreeRuntime(ctx->m3->rt);
    m3_FreeEnvironment(ctx->m3->env);
    WantedFree(ctx->m3);

    DEBUG_TRACE("end");
}

wapp_t *WantedGetCurrentSupervisor() {
    /* TODO: in the future we need to update the image if downloaded new version
     */
    /* now we're using only factory version */

    int ret = 0;
    static wapp_t *w = NULL;

    if (NULL != w) {
        // supervisor metadata is initialized, return it
        return w;
    }

    w = WantedMalloc(sizeof(wapp_t));
    const wantedConfig_t *cfg = WantedGetConfig();

    if (cfg == NULL || !cfg->supervisorCfg.valid) {
        // Load defaults, supervisor config invalid
        const char *def =
#include "default_supervisor_cfg.json.h"
            ;

        DEBUG_TRACE("loading defaults");
        ret =
            WantedParseCtrlActionJson(def, strlen(def), w->name, NULL, &w->cfg);
        if (ret < 0) {
            DEBUG_TRACE("error during loading of defaults: %d", ret);
            return w;
        }
    } else {
        DEBUG_TRACE("using global config");
        w->cfg = cfg->supervisorCfg;
        memcpy(w->name, "supervisor", 11);
    }
    if (ret < 0)
        return w;

    const char *img_path = (cfg && cfg->supervisorImagePath[0])
                               ? cfg->supervisorImagePath
                               : SUPERVISOR_IMAGE_PATH;
    int load_ret = PlatformWappLoad(img_path, w);
    if (load_ret < 0) {
        DEBUG_TRACE("failed to load supervisor image from %s: %d", img_path,
                    load_ret);
    }

    return w;
}

int WantedStart(const char *json, size_t jsonLen) {
    int ret;
    wapp_t *app;

    ret = WantedParseConfig(json, jsonLen);
    if (ret < 0)
        return ret;

    app = WantedGetCurrentSupervisor();

    ret = PlatformWappStart(app);
    if (ret < 0)
        return ret;

    PlatformWappLoop();

    return 0;
}
