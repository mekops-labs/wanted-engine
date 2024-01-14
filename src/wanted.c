#include <errno.h>

#include <wasm3.h>
#include <m3_api_libc.h>
#include <m3_env.h>

#include <tiny-json.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <wanted_malloc.h>
#include <debug_trace.h>

#include <wanted.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <romfs.h>
#include <vfs.h>
#include <vfs-drivers.h>

#include <supervisor.h>

#include <platform.h>

static const char* manifestName = "manifest.json";
static const char* appName      = "app.wasm";

struct m3Data_t {
    m3_wasi_context_t *wasiCtx;
    IM3Runtime rt;
    IM3Environment env;
};

struct wappImgData_t {
    uint8_t *img;
    size_t img_len;
};

static int LoadFile(const char* name, uint8_t *img, size_t imgLen, struct wappImgData_t *file)
{
    int ret, fd;
    romfs_t r;

    if (name == NULL || img == NULL || file == NULL) {
        DEBUG_TRACE("invalid paramter\n");
        return -1;
    }

    ret = RomfsLoad(img, imgLen, &r);
    if (ret < 0) {
        DEBUG_TRACE("RomfsLoad returned %d\n", ret);
        return -1;
    }

    ret = RomfsFdStatAt(r, 3, name, NULL);
    if (ret < 0) {
        DEBUG_TRACE("RomfsFdStatAt returned %d\n", ret);
        goto _exit;
    } else if (!IS_FILE(ret)) {
        DEBUG_TRACE("%s is not correct file\n", name);
        goto _exit;
    }

    fd = RomfsOpenAt(r, 3, name, 0);
    if (fd < 0) {
        DEBUG_TRACE("open returned %d\n", ret);
        goto _close;
    }

    ret = RomfsMapFile(r, (void **)&file->img, &file->img_len, fd, 0);

_close:
    RomfsClose(r, fd);
_exit:
    RomfsUnload(&r);

    return ret;
}

int WantedWappLoadManifest(const wapp_t *w, uint8_t **img, size_t *imgLen)
{
    int ret;
    struct wappImgData_t m;

    ret = LoadFile(manifestName, w->img, w->img_len, &m);
    if (ret < 0) {
        DEBUG_TRACE("Can't load manifest from wapp image: %d", ret);
        return -1;
    }

    *img = m.img;
    *imgLen = m.img_len;

    return 0;
}

int WantedWappParseManifest(wapp_t *w)
{
    int ret = 0;
    json_t mem[32];
    uint8_t *manifestImg;
    size_t manifestLen;

    ret = WantedWappLoadManifest(w, &manifestImg, &manifestLen);
    if (ret < 0) {
        DEBUG_TRACE("Can't load manifest");
        return -1;
    }

    char *buf = WantedMalloc(manifestLen);
    if (buf == NULL) {
        DEBUG_TRACE("Can't allocate mem for manifest json buffer");
        return -1;
    }
    memcpy(buf, manifestImg, manifestLen);

    json_t const* json = json_create(buf, mem, sizeof mem / sizeof *mem );
    if ( !json ) {
        DEBUG_TRACE("Error json create.");
        ret = -1;
        goto _exit;
    }

    json_t const* name = json_getProperty( json, "name" );
    if ( !name || JSON_TEXT != json_getType( name ) ) {
        DEBUG_TRACE("Error, the name property is not found.");
        ret = -1;
        goto _exit;
    }
    strncpy(w->name, json_getValue(name), WAPP_MAX_NAME_LEN);

    json_t const* varsionArr = json_getProperty( json, "version" );
    if ( !varsionArr || JSON_ARRAY != json_getType( varsionArr ) ) {
        DEBUG_TRACE("Error, the version property is not found.");
        ret = -1;
        goto _exit;
    }

    json_t const* version;
    int i;
    for( i = 0, version = json_getChild( varsionArr ); i < 3 && version != 0; i++, version = json_getSibling( version ) ) {
        if ( JSON_INTEGER == json_getType( version ) ) {
            w->version.v[i] = (uint8_t)json_getInteger(version);
        }
    }

    json_t const* pkg = json_getProperty( json, "package" );
    if ( !pkg || JSON_INTEGER != json_getType( pkg ) ) {
        DEBUG_TRACE("Warning, the package property is not found.");
        w->version.package = 0;
    } else {
        w->version.package = (uint8_t)json_getInteger(pkg);
    }

_exit:
    WantedFree(buf);

    return ret;
}

int WantedWappRun(wapp_data_t *ctx)
{
    M3Result status;
    IM3Module mod;
    IM3Function f;
    struct wappImgData_t wasm;
    wapp_t *wapp = ctx->wapp;
    int ret = 0;

    if (ctx == NULL) {
        DEBUG_TRACE("ctx is NULL");
        return -1;
    }

    DEBUG_TRACE("entering thread: %d", ctx->id);

    ret = WantedWappParseManifest(wapp);
    if (ret < 0) {
        DEBUG_TRACE("Can't parse wapp manifest: %d", ret);
        return -1;
    }

    ret = LoadFile(appName, wapp->img, wapp->img_len, &wasm);
    if (ret < 0) {
        DEBUG_TRACE("Can't load application from wapp image: %d", ret);
        return -1;
    }

    ctx->m3 = (struct m3Data_t *)WantedMalloc(sizeof(struct m3Data_t));
    if (!ctx->m3) {
        DEBUG_TRACE("Can't allocate data for m3");
        return -1;
    }
    memset(ctx->m3, 0, sizeof(struct m3Data_t));

    ctx->m3->env = m3_NewEnvironment();
    if (!ctx->m3->env) {
        DEBUG_TRACE("Can't allocate data for m3 env");
        return -1;
    }
    ctx->m3->rt =  m3_NewRuntime(ctx->m3->env, M3_STACK_SIZE, NULL);
    if (!ctx->m3->rt) {
        DEBUG_TRACE("Can't allocate data for m3 rt");
        return -1;
    }

    DEBUG_TRACE("parsing wasm: %p (%zu)", wasm.img, wasm.img_len);
    status = m3_ParseModule(ctx->m3->env, &mod, wasm.img, wasm.img_len);
    if (status) { DEBUG_TRACE("m3_ParseModule[%d]: %s", ctx->id, status); }

    DEBUG_TRACE("loading wasm");
    status = m3_LoadModule(ctx->m3->rt, mod);
    if (status) { DEBUG_TRACE("m3_LoadModule[%d]: %s", ctx->id, status); }

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

    ctx->m3->wasiCtx->argc = 0;
    ctx->m3->wasiCtx->argv = NULL;
    ctx->m3->wasiCtx->vfsCtx = ctx->vfs;

    LinkWASI(mod, ctx->m3->wasiCtx);
    LinkWantedApi(mod);
    m3_LinkLibC(mod);

    /* install console */
    ret =  WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[0].name,   "<stdin>",  wapp->cfg.console[0].options);
    ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[1].name,   "<stdout>", wapp->cfg.console[1].options);
    ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.console[2].name,   "<stderr>", wapp->cfg.console[2].options);

    /* root driver */
    ret += WantedInstallDriver(ctx->vfs, wapp, "virt",       "/", NULL);

    /* fs drivers */
    for (int i = 0; i < wapp->cfg.driversCnt; i++) {
        ret += WantedInstallDriver(ctx->vfs, wapp, wapp->cfg.drivers[i].name, wapp->cfg.drivers[i].path, wapp->cfg.drivers[i].options);
    }

    if (ret < 0) {
        DEBUG_TRACE("error installing drivers");
        goto _freeVfs;
    }

    status = m3_FindFunction (&f, ctx->m3->rt, "entry");
    if (status) {
        status = m3_FindFunction (&f, ctx->m3->rt, "_start");
        if (status) {
            DEBUG_TRACE("m3_FindFunction[%d]: %s", ctx->id, status);
            goto _freeVfs;
        }
    }

    DEBUG_TRACE("starting wapp: %d", ctx->id);
    status = m3_CallV (f, (int32_t)ctx->id);
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
    m3_FreeRuntime(ctx->m3->rt);
    m3_FreeEnvironment(ctx->m3->env);
    WantedFree(ctx->m3);

    DEBUG_TRACE("end");

    return -1;
}

void WantedWappStop(wapp_data_t *ctx)
{
    if (ctx->lastStatus != 0) return;

    DEBUG_TRACE("start");

    VfsDestroy(&ctx->vfs);
    FreeWasiContext(ctx->m3->wasiCtx);
    m3_FreeRuntime(ctx->m3->rt);
    m3_FreeEnvironment(ctx->m3->env);
    WantedFree(ctx->m3);

    DEBUG_TRACE("end");
}

wapp_t *WantedGetCurrentSupervisor()
{
    /* TODO: in the future we need to update the image if downloaded new version */
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
        ret = WantedParseCtrlActionJson(def, strlen(def), w->name, NULL, &w->cfg);
        if (ret < 0) {
            DEBUG_TRACE("error during loading of defaults: %d", ret);
            return w;
        }
    } else {
        DEBUG_TRACE("using global config");
        w->cfg = cfg->supervisorCfg;
        memcpy(w->name, "supervisor", 11);
    }
    if (ret < 0) return w;

    w->img = supervisor_wapp;
    w->img_len = supervisor_wapp_len;

    return w;
}

int WantedStart(const char *json, size_t jsonLen)
{
    int ret;

    ret = WantedParseConfig(json, jsonLen);
    if (ret < 0) return ret;

    ret = PlatformWappStart(WantedGetCurrentSupervisor());
    if (ret < 0) return ret;

    PlatformWappLoop();

    return 0;
}
