#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wasm_export.h>

#include <tiny-json.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <debug_trace.h>
#include <wanted_malloc.h>

#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-pipe.h>
#include <vfs-procfs.h>
#include <vfs-stdio.h>
#include <vfs-tarfs.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <wanted.h>

#include <platform.h>

struct wamrData_t {
    wasm_module_t      module;
    wasm_module_inst_t instance;
    wasm_exec_env_t    exec_env;
    uint8_t           *wasm_bytes; /* writable copy passed to wasm_runtime_load */
};

/* WAMR runtime init is global and one-shot. Called lazily from both
 * WantedStart and WantedWappRun so direct callers (tests) work too. */
static int EnsureWamrInit(void) {
    static bool initialized = false;
    if (initialized)
        return 0;

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        DEBUG_TRACE("wasm_runtime_full_init failed");
        return -1;
    }
    RegisterWASINatives();
    RegisterWantedNatives();
    initialized = true;
    return 0;
}

/* Process-wide pipe store shared by every wapp's /dev/pipe driver — this is
 * what makes named pipes an inter-wapp channel rather than per-wapp scratch.
 * Created lazily on first use, which is the supervisor's thread before any
 * other wapp exists, so the lazy init is race-free. The store lives for the
 * process lifetime (never freed). */
static pipe_store_t *WantedPipeStore(void) {
    static pipe_store_t *store = NULL;
    if (!store)
        store = PipeStoreNew();
    return store;
}

/* /proc/wapps — plain-text wapp state, one record per wapp. */
static int ProcReadWapps(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    wapp_state_t wapps[MAX_WAPPS];
    int n = PlatformWappGetState(wapps, MAX_WAPPS);
    if (n < 0)
        return n;

    char *p = (char *)buf;
    size_t left = bufLen;
    for (int i = 0; i < n && left > 0; i++) {
        int w = snprintf(p, left, "name:\t%s\nstate:\t%s\n",
                         wapps[i].name, statusToString(wapps[i].status));
        if (w < 0 || (size_t)w >= left)
            break;
        p += w;
        left -= (size_t)w;
        if (i + 1 < n && left > 1) {
            *p++ = '\n';
            left--;
        }
    }
    return (int)(bufLen - left);
}

/* /proc/memory — wasm stack size + platform heap via PlatformMemoryStats. */
static int ProcReadMemory(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    size_t heap_used = 0, heap_total = 0;
    PlatformMemoryStats(&heap_used, &heap_total);
    int w = snprintf((char *)buf, bufLen,
                     "stack_size:\t%d B\nheap_used:\t%zu B\nheap_total:\t%zu B\n",
                     WASM_STACK_SIZE, heap_used, heap_total);
    if (w < 0)
        return -EIO;
    return w < (int)bufLen ? w : (int)bufLen;
}

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
    json_t mem[48];

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

    json_t const *reqs = json_getProperty(json, "requirements");
    if (reqs && JSON_ARRAY == json_getType(reqs)) {
        json_t const *r;
        int ri = 0;
        for (r = json_getChild(reqs);
             r && ri < WAPP_MAX_REQUIREMENTS;
             r = json_getSibling(r), ri++) {
            if (JSON_TEXT == json_getType(r)) {
                strncpy(w->requirements[ri], json_getValue(r),
                        WAPP_MAX_REQ_NAME_LEN - 1);
                w->requirements[ri][WAPP_MAX_REQ_NAME_LEN - 1] = '\0';
            }
        }
        w->requirementsCnt = (uint8_t)ri;
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
    wasm_function_inst_t f = NULL;
    wapp_t *wapp;
    vfs_tarfs_ctx_t *tarfs = NULL;
    const uint8_t *manifest = NULL;
    const uint8_t *wasm = NULL;
    size_t manifestLen = 0, wasmLen = 0;
    char err_buf[128];
    wasi_ctx_t *wasiCtx = NULL;
    int ret = 0;

    if (ctx == NULL) {
        DEBUG_TRACE("ctx is NULL");
        return -1;
    }

    if (EnsureWamrInit() < 0)
        return -1;

    /* WAMR's hardware bound check installs per-thread SIGSEGV trap
     * environment. Each worker thread that calls into wasm must initialise
     * its own env (idempotent if already done). */
    if (!wasm_runtime_init_thread_env()) {
        DEBUG_TRACE("wasm_runtime_init_thread_env failed");
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

    ctx->wamr = (struct wamrData_t *)WantedMalloc(sizeof(struct wamrData_t));
    if (!ctx->wamr) {
        DEBUG_TRACE("Can't allocate wamrData_t");
        ret = -1;
        goto _freeTarfs;
    }
    memset(ctx->wamr, 0, sizeof(struct wamrData_t));

    /* WAMR's loader may modify the buffer in-place (LEB128 patching) and
     * holds references into it for the module's lifetime. TarFS-mapped layer
     * memory is shared with other consumers and unsafe to mutate, so copy
     * to a heap buffer freed after wasm_runtime_unload. */
    ctx->wamr->wasm_bytes = WantedMalloc(wasmLen);
    if (!ctx->wamr->wasm_bytes) {
        DEBUG_TRACE("Can't allocate writable wasm buffer");
        ret = -1;
        goto _freeWamr;
    }
    memcpy(ctx->wamr->wasm_bytes, wasm, wasmLen);

    DEBUG_TRACE("loading wasm: %p (%zu)", ctx->wamr->wasm_bytes, wasmLen);
    ctx->wamr->module = wasm_runtime_load(ctx->wamr->wasm_bytes,
                                          (uint32_t)wasmLen,
                                          err_buf, sizeof(err_buf));
    if (!ctx->wamr->module) {
        DEBUG_TRACE("wasm_runtime_load[%d]: %s", ctx->id, err_buf);
        ret = -1;
        goto _freeWasmBytes;
    }

    ctx->wamr->instance = wasm_runtime_instantiate(ctx->wamr->module,
                                                   WASM_STACK_SIZE,
                                                   WASM_HEAP_SIZE,
                                                   err_buf, sizeof(err_buf));
    if (!ctx->wamr->instance) {
        DEBUG_TRACE("wasm_runtime_instantiate[%d]: %s", ctx->id, err_buf);
        ret = -1;
        goto _unloadModule;
    }

    ctx->wamr->exec_env = wasm_runtime_create_exec_env(ctx->wamr->instance,
                                                       WASM_STACK_SIZE);
    if (!ctx->wamr->exec_env) {
        DEBUG_TRACE("wasm_runtime_create_exec_env[%d] failed", ctx->id);
        ret = -1;
        goto _deinstantiate;
    }

    DEBUG_TRACE("getting context");
    wasiCtx = InitWasiContext();
    if (!wasiCtx) {
        DEBUG_TRACE("InitWasiContext: can't allocate");
        ret = -1;
        goto _destroyExecEnv;
    }

    ctx->vfs = VfsInit();
    if (!ctx->vfs) {
        DEBUG_TRACE("VfsInit: can't allocate");
        ret = -1;
        goto _freeWasiCtx;
    }

    /* Hand the tarfs ctx off — VfsDestroy now frees it. */
    VfsAttachTarfs(ctx->vfs, tarfs);
    tarfs = NULL;

    /* Builtin /dev entries — always present regardless of wapp manifest. */
    DevFs_Register(ctx->vfs, "null",   VfsNullInit(wapp, NULL));
    DevFs_Register(ctx->vfs, "pipe",   PipeDriverCreate(WantedPipeStore()));
    DevFs_Register(ctx->vfs, "stdin",  VfsStdinDriverGet());
    DevFs_Register(ctx->vfs, "stdout", VfsStdoutDriverGet());
    DevFs_Register(ctx->vfs, "stderr", VfsStderrDriverGet());

    /* Propagate system-level privilege flag, then register /proc entries. */
    VfsSetPrivileged(ctx->vfs, WantedGetConfig()->privileged);
    ProcFs_Register(ctx->vfs, "wapps",  ProcReadWapps,  true);
    ProcFs_Register(ctx->vfs, "memory", ProcReadMemory, true);
    /* clock_quality is unprivileged — any wapp may read it to decide whether
     * to trust the wall clock. */
    ProcFs_Register(ctx->vfs, "clock_quality",
                    WantedProcReadClockQuality, false);

    wasiCtx->argc   = 0;
    wasiCtx->argv   = NULL;
    wasiCtx->vfsCtx = ctx->vfs;
    wasm_runtime_set_user_data(ctx->wamr->exec_env, wasiCtx);

    /* Persistent-state preopens: any wapp can declare host directories via
     * params.preopens in its launch config. The Engine creates each (if
     * absent), opens it, and exposes it to the wapp as a WASI preopen at the
     * same path. Failures are non-fatal — the wapp will surface a missing-
     * preopen error itself if the state is actually required. */
    for (size_t pi = 0; pi < wapp->cfg.preopensCnt; pi++) {
        const char *p = wapp->cfg.preopens[pi];
        int host_fd = PlatformOpenStateDir(p);
        if (host_fd < 0) {
            DEBUG_TRACE("PlatformOpenStateDir(%s) failed: %d", p, host_fd);
            continue;
        }
        int rc = WasiCtxAddPreopen(wasiCtx, p, host_fd);
        if (rc < 0) {
            DEBUG_TRACE("WasiCtxAddPreopen(%s) failed: %d", p, rc);
        }
    }

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

    f = wasm_runtime_lookup_function(ctx->wamr->instance, "entry");
    if (!f)
        f = wasm_runtime_lookup_function(ctx->wamr->instance, "_start");
    if (!f) {
        DEBUG_TRACE("wasm_runtime_lookup_function[%d]: entry/_start not found",
                    ctx->id);
        goto _freeVfs;
    }

    DEBUG_TRACE("starting wapp: %d", ctx->id);
    uint32_t argv[1] = {(uint32_t)ctx->id};
    if (!wasm_runtime_call_wasm(ctx->wamr->exec_env, f, 1, argv)) {
        const char *exc = wasm_runtime_get_exception(ctx->wamr->instance);
        if (exc && strcmp(exc, "proc_exit") == 0) {
            DEBUG_TRACE("normal exit via proc_exit");
        } else {
            DEBUG_TRACE("wasm_runtime_call_wasm[%d]: %s", ctx->id,
                        exc ? exc : "(no exception)");
        }
    }

    DEBUG_TRACE("normal exit");

    return 0;

_freeVfs:
    VfsDestroy(&ctx->vfs);
_freeWasiCtx:
    FreeWasiContext(wasiCtx);
_destroyExecEnv:
    wasm_runtime_destroy_exec_env(ctx->wamr->exec_env);
_deinstantiate:
    wasm_runtime_deinstantiate(ctx->wamr->instance);
_unloadModule:
    wasm_runtime_unload(ctx->wamr->module);
_freeWasmBytes:
    WantedFree(ctx->wamr->wasm_bytes);
_freeWamr:
    WantedFree(ctx->wamr);
_freeTarfs:
    if (tarfs)
        TarFsDestroy(tarfs);

    DEBUG_TRACE("end");

    return ret < 0 ? ret : -1;
}

void WantedWappStop(wapp_data_t *ctx) {
    /* Instance teardown only applies to a wapp that ran to completion; on the
     * failure path WantedWappRun has already unwound the WAMR instance. */
    if (ctx->lastStatus == 0) {
        DEBUG_TRACE("start");

        VfsDestroy(&ctx->vfs);

        wasi_ctx_t *wasiCtx =
            (wasi_ctx_t *)wasm_runtime_get_user_data(ctx->wamr->exec_env);
        FreeWasiContext(wasiCtx);

        wasm_runtime_destroy_exec_env(ctx->wamr->exec_env);
        wasm_runtime_deinstantiate(ctx->wamr->instance);
        wasm_runtime_unload(ctx->wamr->module);
        WantedFree(ctx->wamr->wasm_bytes);
        WantedFree(ctx->wamr);

        DEBUG_TRACE("end");
    }

    /* Tear down the per-thread WAMR env that WantedWappRun initialised on this
     * worker thread. This runs on every exit (success or failure) and is the
     * symmetric counterpart to wasm_runtime_init_thread_env(): it restores the
     * stack guard pages this thread mprotect()'d to PROT_NONE and frees its
     * signal alternate stack. Skipping it leaves a detached thread's (glibc-
     * cached, later reused) stack with unreadable guard pages — the next
     * worker's init_thread_env then faults touching them. The call self-guards
     * when no env was initialised, so it is safe on the early-failure path. */
    wasm_runtime_destroy_thread_env();
}

void WantedWappTerminate(wapp_data_t *ctx) {
    /* Signal a running instance to abort its in-flight WASM execution:
     * wasm_runtime_call_wasm on the worker thread returns false, so the thread
     * unwinds through its cleanup handler and WantedWappStop. This is the stop
     * path for platforms that cannot rely on forced thread cancellation.
     * Self-guards when the wapp has no live instance. */
    if (ctx == NULL || ctx->wamr == NULL || ctx->wamr->instance == NULL) {
        return;
    }
    wasm_runtime_terminate(ctx->wamr->instance);
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

    if (EnsureWamrInit() < 0)
        return -1;

    app = WantedGetCurrentSupervisor();

    ret = PlatformWappStart(app);
    if (ret < 0)
        return ret;

    PlatformWappLoop();

    return 0;
}
