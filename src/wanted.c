/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <wasm_export.h>

#include <wanted_wasm_api.h>
#include <wasi.h>

#include <debug_trace.h>
#include <wanted-autoconf.h>
#include <wanted_log.h>
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
    wasm_module_t module;
    wasm_module_inst_t instance;
    wasm_exec_env_t exec_env;
    uint8_t *wasm_bytes; /* writable copy passed to wasm_runtime_load */
};

void WantedWappMemStats(const struct wamrData_t *wamr, wapp_state_t *out) {
    out->mem_pages_cur = 0;
    out->mem_pages_max = 0;
    out->mem_bytes_cur = 0;
    out->mem_bytes_max = 0;
    if (wamr == NULL || wamr->instance == NULL)
        return;

    wasm_memory_inst_t mem = wasm_runtime_get_default_memory(wamr->instance);
    if (mem == NULL)
        return; /* a module that declares no linear memory */

    uint64_t pages_cur = wasm_memory_get_cur_page_count(mem);
    uint64_t pages_max = wasm_memory_get_max_page_count(mem);
    uint64_t bytes_per_page = wasm_memory_get_bytes_per_page(mem);

    out->mem_pages_cur = (uint32_t)pages_cur;
    out->mem_pages_max = (uint32_t)pages_max;
    out->mem_bytes_cur = (size_t)(pages_cur * bytes_per_page);
    out->mem_bytes_max = (size_t)(pages_max * bytes_per_page);
}

/* Default backing for a stdio slot the config leaves unset: `null` for stdin,
 * `log` for stdout/stderr, so output is captured rather than silently lost.
 * All three fds must be wired or the wapp fails to launch. */
#define DEFAULT_CONSOLE_IN "null"
#define DEFAULT_CONSOLE_OUT "log"
#define DEFAULT_CONSOLE_ERR "log"

/* Launch-config resource mount templates. A device singleton (drivers[]) mounts
 * at /dev/<name>; a socket (sockets[]) is created at /net/<name>. */
#define WANTED_DEV_MOUNT_FMT "/dev/%s"
#define WANTED_NET_MOUNT_FMT "/net/%s"

/* Namespace segments for engine-managed volumes. The fixed `priv`/`shared`
 * segments keep the private and shared namespaces disjoint, so no wapp name
 * can collide with a shared volume name. */
#define WANTED_VOLUME_DEFAULT_NAME "default"
#define WANTED_VOLUME_PRIV_FMT "%s/priv/%s/%s"  /* <root>/priv/<wapp>/<vol> */
#define WANTED_VOLUME_SHARED_FMT "%s/shared/%s" /* <root>/shared/<vol> */

/* A console slot with no driver name falls back to its default backing. */
static const char *resolveConsole(const char *name, const char *fallback) {
    return (name == NULL || name[0] == '\0') ? fallback : name;
}

static pipe_store_t *wantedPipeStore(void);

/* Per-slot label used to auto-name a `pipe` console's pipe (<wapp>.<slot>). */
static const char *const CONSOLE_SLOT[] = {"in", "out", "err"};

/* Pull a `name=<pipe>` value out of a console slot's comma-separated options
 * into `out`; returns true when present. Lets a config pin the console pipe's
 * name (e.g. for a fixed reader) instead of the derived <wapp>.<slot>. */
static bool consolePipeName(const char *opt, char *out, size_t cap) {
    if (opt == NULL)
        return false;
    const char *p = opt;
    while (*p != '\0') {
        if (strncmp(p, "name=", 5) == 0) {
            p += 5;
            size_t n = 0;
            while (p[n] != '\0' && p[n] != ',' && n < cap - 1)
                n++;
            memcpy(out, p, n);
            out[n] = '\0';
            return n > 0;
        }
        while (*p != '\0' && *p != ',')
            p++;
        if (*p == ',')
            p++;
    }
    return false;
}

/* Install one console slot. A `pipe` backing auto-creates a named pipe in the
 * shared store and binds the stream fd to it; every other backing resolves
 * through the driver table. */
static int installConsoleSlot(wapp_data_t *ctx, const wapp_t *wapp, int idx,
                              const char *fallback, const char *path) {
    const char *name = resolveConsole(wapp->cfg.console[idx].name, fallback);
    const char *options = wapp->cfg.console[idx].options;

    if (strcmp(name, "pipe") == 0) {
        char pname[WAPP_MAX_NAME_LEN + 8];
        if (!consolePipeName(options, pname, sizeof(pname)))
            snprintf(pname, sizeof(pname), "%s.%s", wapp->name,
                     CONSOLE_SLOT[idx]);
        const vfs_driver_t *drv = VfsPipeConsoleCreate(wantedPipeStore(), pname,
                                                       idx == 0, VFS_O_RDONLY);
        if (drv == NULL)
            return -ENOMEM;
        return VfsRegister(ctx->vfs, path, drv);
    }

    return WantedInstallDriver(ctx->vfs, wapp, name, path, options);
}

/* True when `path` is the reserved namespace `ns` itself or a path beneath it
 * ("/dev", "/dev/x"), without matching unrelated names that merely share the
 * prefix ("/development"). Used to keep mounts[] out of /dev and /net. */
static bool isReservedNamespace(const char *path, const char *ns) {
    size_t n = strlen(ns);
    return strncmp(path, ns, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Parse a `platform` mount's options: `src=<abshostpath>` and `ro`/`rw`.
 * `hostBuf` receives src (empty when unset), `*readonly` the access mode.
 * A relative, empty or oversized src, or an unknown token, gives -EINVAL. */
static int parsePlatformMountOptions(const char *options, char *hostBuf,
                                     size_t hostBufLen, bool *readonly) {
    *readonly = false;
    hostBuf[0] = '\0';
    if (options == NULL || options[0] == '\0')
        return 0;

    char buf[CONFIG_WANTED_MAX_OPTIONS_SIZE];
    size_t olen = strnlen(options, sizeof(buf));
    if (olen >= sizeof(buf))
        return -EINVAL;
    memcpy(buf, options, olen + 1);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        if (strncmp(tok, "src=", 4) == 0) {
            const char *src = tok + 4;
            if (src[0] != '/') /* must be an absolute host path */
                return -EINVAL;
            size_t slen = strlen(src);
            if (slen >= hostBufLen)
                return -EINVAL;
            memcpy(hostBuf, src, slen + 1);
        } else if (strcmp(tok, "ro") == 0) {
            *readonly = true;
        } else if (strcmp(tok, "rw") == 0) {
            *readonly = false;
        } else {
            return -EINVAL;
        }
    }
    return 0;
}

/* A volume name must be a single safe path component: non-empty, free of '/',
 * and not "." or "..". It is concatenated into a host path, so a separator or
 * parent ref would escape the wapp's volume namespace. */
static bool isSafeVolumeName(const char *name) {
    if (name == NULL || name[0] == '\0')
        return false;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return false;
    return strchr(name, '/') == NULL;
}

/* Parse a `volume` mount's options: `name=<volname>` (default "default"),
 * `ro`/`rw` (default rw), and `shared` (default private). An unsafe or
 * oversized name, or an unknown token, gives -EINVAL. */
static int parseVolumeMountOptions(const char *options, char *nameBuf,
                                   size_t nameBufLen, bool *readonly,
                                   bool *shared) {
    *readonly = false;
    *shared = false;
    snprintf(nameBuf, nameBufLen, "%s", WANTED_VOLUME_DEFAULT_NAME);
    if (options == NULL || options[0] == '\0')
        return 0;

    char buf[CONFIG_WANTED_MAX_OPTIONS_SIZE];
    size_t olen = strnlen(options, sizeof(buf));
    if (olen >= sizeof(buf))
        return -EINVAL;
    memcpy(buf, options, olen + 1);

    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok != NULL;
         tok = strtok_r(NULL, ",", &save)) {
        if (strncmp(tok, "name=", 5) == 0) {
            const char *vn = tok + 5;
            if (!isSafeVolumeName(vn))
                return -EINVAL;
            size_t vlen = strlen(vn);
            if (vlen >= nameBufLen)
                return -EINVAL;
            memcpy(nameBuf, vn, vlen + 1);
        } else if (strcmp(tok, "ro") == 0) {
            *readonly = true;
        } else if (strcmp(tok, "rw") == 0) {
            *readonly = false;
        } else if (strcmp(tok, "shared") == 0) {
            *shared = true;
        } else {
            return -EINVAL;
        }
    }
    return 0;
}

/* Build the WASI argv/envp pointer arrays; argv[0] is the wapp name. Strings
 * point into the wapp's persistent launch-config storage, which outlives the
 * wasi context, so none are copied. FreeWasiContext releases the arrays. */
static int buildWasiArgs(wasi_ctx_t *wasiCtx, const wapp_t *wapp) {
    uint32_t argc = 1 + (uint32_t)wapp->cfg.argsCnt;
    const char **argv = (const char **)WantedMalloc(argc * sizeof(char *));
    if (!argv)
        return -ENOMEM;
    argv[0] = wapp->name;
    for (size_t i = 0; i < wapp->cfg.argsCnt; i++)
        argv[1 + i] = wapp->cfg.args[i];

    const char **envp = NULL;
    uint32_t envc = (uint32_t)wapp->cfg.envsCnt;
    if (envc > 0) {
        envp = (const char **)WantedMalloc(envc * sizeof(char *));
        if (!envp) {
            WantedFree((void *)argv);
            return -ENOMEM;
        }
        for (size_t i = 0; i < wapp->cfg.envsCnt; i++)
            envp[i] = wapp->cfg.envs[i];
    }

    wasiCtx->argc = argc;
    wasiCtx->argv = argv;
    wasiCtx->envc = envc;
    wasiCtx->envp = envp;
    return 0;
}

/* WAMR runtime init is global and one-shot. Called lazily from both
 * WantedStart and WantedWappRun so direct callers (tests) work too. */
/* WAMR custom-allocator hooks routing runtime and linear-memory allocation to
 * external RAM, keeping internal RAM for task stacks. The signature varies with
 * WASM_MEM_ALLOC_WITH_USAGE, which the NuttX build enables. */
static void *wamrMalloc(unsigned int size) {
    return PlatformExtramMalloc(size);
}
static void *wamrRealloc(void *ptr, unsigned int size) {
    return PlatformExtramRealloc(ptr, size);
}
static void wamrFree(void *ptr) { PlatformExtramFree(ptr); }

static int ensureWamrInit(void) {
    static bool initialized = false;
    if (initialized)
        return 0;

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    /* Allocate WAMR's runtime and linear memory from external RAM so internal
     * RAM is left for worker task stacks, which can only live there. Without
     * external RAM these resolve to the ordinary heap. */
    init_args.mem_alloc_type = Alloc_With_Allocator;
    init_args.mem_alloc_option.allocator.malloc_func = (void *)wamrMalloc;
    init_args.mem_alloc_option.allocator.realloc_func = (void *)wamrRealloc;
    init_args.mem_alloc_option.allocator.free_func = (void *)wamrFree;
    if (!wasm_runtime_full_init(&init_args)) {
        DEBUG_TRACE("wasm_runtime_full_init failed");
        return -1;
    }
    RegisterWASINatives();
    RegisterWantedNatives();
    initialized = true;
    return 0;
}

/* Process-wide pipe store shared by every wapp's /dev/pipe driver, which is
 * what makes a named pipe an inter-wapp channel. First use is the supervisor's
 * thread before any other wapp exists, so the lazy init is race-free. */
static pipe_store_t *wantedPipeStore(void) {
    static pipe_store_t *store = NULL;
    if (!store)
        store = PipeStoreNew();
    return store;
}

/* /proc/memory — wasm stack size + platform heap via PlatformMemoryStats. */
static int procReadMemory(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    size_t heap_used = 0, heap_total = 0;
    size_t store_free = 0, store_total = 0;
    PlatformMemoryStats(&heap_used, &heap_total);
    PlatformStorageStats(&store_free, &store_total);

    /* Free WASM linear-memory pages: the sum, across every loaded wapp, of the
     * headroom left before its own page ceiling. Live per-wapp figures already
     * back /proc/wapps/<name>/memory; this aggregates the same sample. */
    wapp_state_t states[CONFIG_WANTED_MAX_WAPPS];
    int n = PlatformWappGetState(states, CONFIG_WANTED_MAX_WAPPS);
    uint64_t wasm_pages_free = 0;
    for (int i = 0; i < n; i++) {
        if (states[i].mem_pages_max > states[i].mem_pages_cur)
            wasm_pages_free +=
                states[i].mem_pages_max - states[i].mem_pages_cur;
    }

    int w =
        snprintf((char *)buf, bufLen,
                 "stack_size:\t%d B\nheap_used:\t%zu B\nheap_total:\t%zu B\n"
                 "store_free:\t%zu B\nstore_total:\t%zu B\n"
                 "wasm_pages_free:\t%llu\n",
                 CONFIG_WANTED_WASM_STACK_SIZE, heap_used, heap_total,
                 store_free, store_total, (unsigned long long)wasm_pages_free);
    if (w < 0)
        return -EIO;
    return w < (int)bufLen ? w : (int)bufLen;
}

#ifndef WANTED_VERSION
#define WANTED_VERSION "unknown"
#endif

/* /proc/wanted — engine identity and compile-time resource ceilings. One
 * key:\tvalue line per field: human-readable, trivially split on the tab. */
static int procReadWanted(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    int w = snprintf(
        (char *)buf, bufLen,
        "platform:\t%s\n"
        "version:\t%s\n"
        "supervisor_abi:\t%d\n"
        "max_wapps:\t%d\n"
        "max_wapp_name:\t%d B\n"
        "max_path:\t%d B\n"
        "wasm_stack:\t%d B\n"
        "wasm_heap:\t%d B\n"
        "wasm_worker_stack:\t%zu B\n"
        "wasm_max_pages:\t%d\n"
        "max_drivers:\t%d\n"
        "max_options:\t%d B\n"
        "log_slots:\t%d\n"
        "reg_slots:\t%zu\n",
        PlatformName(), WANTED_VERSION, WANTED_SUPERVISOR_ABI,
        CONFIG_WANTED_MAX_WAPPS, WAPP_MAX_NAME_LEN, CONFIG_WANTED_MAX_PATH_LEN,
        CONFIG_WANTED_WASM_STACK_SIZE, CONFIG_WANTED_WASM_HEAP_SIZE,
        PlatformWorkerStackSize(), CONFIG_WANTED_WASM_MAX_MEMORY_PAGES,
        CONFIG_WANTED_MAX_DRIVERS_CNT, CONFIG_WANTED_MAX_OPTIONS_SIZE,
        CONFIG_WANTED_LOG_SLOTS, PlatformRegistrySlots());
    if (w < 0)
        return -EIO;

    /* Build-time image digest, on a platform that stamps one. The line is
     * absent elsewhere, so a reader never mistakes an empty value for an
     * image that hashes to nothing. */
    char digest[FIRMWARE_DIGEST_HEX_LEN + 1];
    if (w < (int)bufLen && PlatformFirmwareDigest(digest, sizeof(digest)) > 0) {
        int n = snprintf((char *)buf + w, (size_t)((int)bufLen - w),
                         "digest:\t%s\n", digest);
        if (n > 0 && w + n < (int)bufLen)
            w += n;
    }

    /* Available drivers: the merged core + platform table a launch config can
     * request on this build, so a supervisor can discover capability before
     * configuring a wapp. */
    if (w < (int)bufLen) {
        int n =
            snprintf((char *)buf + w, (size_t)((int)bufLen - w), "drivers:\t");
        if (n > 0 && w + n < (int)bufLen) {
            w += n;
            int d =
                WantedListDrivers((char *)buf + w, (size_t)((int)bufLen - w));
            if (d > 0)
                w += d;
            if (w < (int)bufLen)
                w += snprintf((char *)buf + w, (size_t)((int)bufLen - w), "\n");
        }
    }

    return w < (int)bufLen ? w : (int)bufLen;
}

/* Build a one-shot tarfs ctx over the wapp's layer stack. Caller owns the
 * returned ctx and must TarFsDestroy() it. Returns NULL on bad args / OOM /
 * malformed layer. */
static vfs_tarfs_ctx_t *wappTarfsInit(const wapp_t *w) {
    if (!w || w->layer_cnt == 0)
        return NULL;
    return TarFsInit((uint8_t *const *)w->layers, w->layer_lens, w->layer_cnt);
}

#if CONFIG_WANTED_WASM_MAX_MEMORY_PAGES > 0
/* Largest declared initial linear-memory page count among a module's memories,
 * or 0 if none. WAMR's max_memory_pages bounds only memory.grow and is clamped
 * up to the initial pages, so this check refuses an image that sidesteps it. */
static uint32_t wappInitMemoryPages(wasm_module_t module) {
    uint32_t pages = 0, init;
    int32_t i, n;

    n = wasm_runtime_get_export_count(module);
    for (i = 0; i < n; i++) {
        wasm_export_t e;
        wasm_runtime_get_export_type(module, i, &e);
        if (e.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            init = wasm_memory_type_get_init_page_count(e.u.memory_type);
            if (init > pages)
                pages = init;
        }
    }
    n = wasm_runtime_get_import_count(module);
    for (i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(module, i, &im);
        if (im.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            init = wasm_memory_type_get_init_page_count(im.u.memory_type);
            if (init > pages)
                pages = init;
        }
    }
    return pages;
}

/* The module's own declared max linear pages, or 0 when it declares none. */
static uint32_t wappMaxMemoryPages(wasm_module_t module) {
    uint32_t pages = 0, max;
    int32_t i, n;

    n = wasm_runtime_get_export_count(module);
    for (i = 0; i < n; i++) {
        wasm_export_t e;
        wasm_runtime_get_export_type(module, i, &e);
        if (e.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            max = wasm_memory_type_get_max_page_count(e.u.memory_type);
            if (max > pages)
                pages = max;
        }
    }
    n = wasm_runtime_get_import_count(module);
    for (i = 0; i < n; i++) {
        wasm_import_t im;
        wasm_runtime_get_import_type(module, i, &im);
        if (im.kind == WASM_IMPORT_EXPORT_KIND_MEMORY) {
            max = wasm_memory_type_get_max_page_count(im.u.memory_type);
            if (max > pages)
                pages = max;
        }
    }
    return pages;
}
#endif

int WantedWappRun(wapp_data_t *ctx) {
    wasm_function_inst_t f = NULL;
    wapp_t *wapp;
    vfs_tarfs_ctx_t *tarfs = NULL;
    const uint8_t *wasm = NULL;
    size_t wasmLen = 0;
    char err_buf[128];
    wasi_ctx_t *wasiCtx = NULL;
    int ret = 0;

    if (ctx == NULL) {
        DEBUG_TRACE("ctx is NULL");
        return -1;
    }

    if (ensureWamrInit() < 0)
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

    /* Build the per-wapp tarfs index once. The pre-fetched entrypoint feeds
     * the wasm load below, and the same ctx is attached to the vfs so the
     * prefix router resolves paths into the same layer stack. */
    tarfs = wappTarfsInit(wapp);
    if (!tarfs) {
        DEBUG_TRACE("Can't initialize tarfs for wapp");
        return -1;
    }

    wasm = TarFsEntrypointWasm(tarfs, &wasmLen);
    if (!wasm) {
        LOG_ERROR("wapp '%s': app.wasm absent from the image", wapp->name);
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

    /* WAMR's loader patches the buffer in place and holds references into it
     * for the module's lifetime. TarFS layer memory is shared with other
     * consumers, so copy to a heap buffer freed after wasm_runtime_unload. */
    ctx->wamr->wasm_bytes = PlatformExtramMalloc(wasmLen);
    if (!ctx->wamr->wasm_bytes) {
        DEBUG_TRACE("Can't allocate writable wasm buffer");
        ret = -1;
        goto _freeWamr;
    }
    memcpy(ctx->wamr->wasm_bytes, wasm, wasmLen);

    DEBUG_TRACE("loading wasm: %p (%zu)", ctx->wamr->wasm_bytes, wasmLen);
    ctx->wamr->module = wasm_runtime_load(
        ctx->wamr->wasm_bytes, (uint32_t)wasmLen, err_buf, sizeof(err_buf));
    if (!ctx->wamr->module) {
        LOG_ERROR("wapp '%s': cannot load app.wasm: %s", wapp->name, err_buf);
        ret = -1;
        goto _freeWasmBytes;
    }

    uint32_t max_pages = CONFIG_WANTED_WASM_MAX_MEMORY_PAGES;
#if CONFIG_WANTED_WASM_MAX_MEMORY_PAGES > 0
    /* Refuse an image whose declared initial linear memory already exceeds the
     * per-wapp cap (the runtime cap only bounds later growth). */
    {
        uint32_t init_pages = wappInitMemoryPages(ctx->wamr->module);
        if (init_pages > CONFIG_WANTED_WASM_MAX_MEMORY_PAGES) {
            LOG_ERROR("wapp '%s': initial memory %u pages exceeds the %u-page "
                      "cap",
                      wapp->name, init_pages,
                      CONFIG_WANTED_WASM_MAX_MEMORY_PAGES);
            ret = -1;
            goto _unloadModule;
        }
        /* The cap only lowers: the runtime rejects an override above the
         * module's declared max. */
        uint32_t mod_max = wappMaxMemoryPages(ctx->wamr->module);
        if (mod_max > 0 && mod_max < max_pages)
            max_pages = mod_max;
    }
#endif

    InstantiationArgs inst_args;
    memset(&inst_args, 0, sizeof(inst_args));
    inst_args.default_stack_size = CONFIG_WANTED_WASM_STACK_SIZE;
    inst_args.host_managed_heap_size = CONFIG_WANTED_WASM_HEAP_SIZE;
    /* Cap linear-memory growth per wapp (0 = use the module's declared max). */
    inst_args.max_memory_pages = max_pages;
    ctx->wamr->instance = wasm_runtime_instantiate_ex(
        ctx->wamr->module, &inst_args, err_buf, sizeof(err_buf));
    if (!ctx->wamr->instance) {
        LOG_ERROR("wapp '%s': cannot instantiate: %s", wapp->name, err_buf);
        ret = -1;
        goto _unloadModule;
    }

    ctx->wamr->exec_env = wasm_runtime_create_exec_env(
        ctx->wamr->instance, CONFIG_WANTED_WASM_STACK_SIZE);
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

    /* Builtin /dev entries — always present regardless of wapp config. The
     * stdin/stdout/stderr aliases are registered after the console is installed
     * (below), so they can forward to the wapp's actual stream backing. */
    DevFs_Register(ctx->vfs, "null", VfsNullInit(wapp, NULL));
    DevFs_Register(ctx->vfs, "pipe", PipeDriverCreate(wantedPipeStore()));

    /* Propagate system-level privilege flag, then register /proc entries. */
    VfsSetPrivileged(ctx->vfs, WantedGetConfig()->privileged);
    /* A blocking driver watches this beside its own resource, so a stop reaches
     * a wapp parked in a host call. */
    VfsSetWakeFd(ctx->vfs, PlatformWakeCreate());
    ProcFs_RegisterDir(ctx->vfs, "wapps", &WappsProcDirOps, true);
    ProcFs_Register(ctx->vfs, "memory", procReadMemory, true);
    /* clock_quality is unprivileged — any wapp may read it to decide whether
     * to trust the wall clock. */
    ProcFs_Register(ctx->vfs, "clock_quality", WantedProcReadClockQuality,
                    false);
    /* wanted exposes engine identity and resource ceilings; unprivileged so any
     * wapp can introspect the host it runs on. */
    ProcFs_Register(ctx->vfs, "wanted", procReadWanted, false);

    wasiCtx->vfsCtx = ctx->vfs;

    /* Pass the launch config's args/envs through as WASI argv/envp. argv[0] is
     * the wapp name; user args occupy argv[1..]. */
    if (buildWasiArgs(wasiCtx, wapp) < 0) {
        DEBUG_TRACE("buildWasiArgs: can't allocate argv/envp");
        ret = -1;
        goto _freeVfs;
    }
    wasm_runtime_set_user_data(ctx->wamr->exec_env, wasiCtx);

    /* install console (an unset slot falls back to its default backing) */
    ret = installConsoleSlot(ctx, wapp, 0, DEFAULT_CONSOLE_IN, "<stdin>");
    ret += installConsoleSlot(ctx, wapp, 1, DEFAULT_CONSOLE_OUT, "<stdout>");
    ret += installConsoleSlot(ctx, wapp, 2, DEFAULT_CONSOLE_ERR, "<stderr>");

    /* /dev/std{in,out,err} alias the just-installed console streams — opening
     * the /dev path reaches the same backing as the matching WASI fd (0/1/2).
     */
    DevFs_Register(
        ctx->vfs, "stdin",
        VfsStdioAliasInit(VfsStreamDriver(ctx->vfs, VFS_STDIN), VFS_STDIN));
    DevFs_Register(
        ctx->vfs, "stdout",
        VfsStdioAliasInit(VfsStreamDriver(ctx->vfs, VFS_STDOUT), VFS_STDOUT));
    DevFs_Register(
        ctx->vfs, "stderr",
        VfsStdioAliasInit(VfsStreamDriver(ctx->vfs, VFS_STDERR), VFS_STDERR));

    /* drivers[]: device singletons. Each mounts at /dev/<name>; the name alone
     * determines the mount, so a config-supplied path is meaningless and
     * rejected. */
    for (size_t i = 0; i < wapp->cfg.driversCnt; i++) {
        const wapp_driver_t *d = &wapp->cfg.drivers[i];
        if (d->path[0] != '\0') {
            DEBUG_TRACE("drivers[%zu] '%s': path not allowed", i, d->name);
            ret += -EINVAL;
            continue;
        }
        char mount[CONFIG_WANTED_MAX_PATH_LEN];
        snprintf(mount, sizeof(mount), WANTED_DEV_MOUNT_FMT, d->name);
        ret += WantedInstallDriver(ctx->vfs, wapp, d->name, mount, d->options);
    }

    /* sockets[]: named sockets created at /net/<name>. The transport spec is
     * the entry's address (carried in options, with its role and listen bounds
     * appended); a config-supplied path is rejected. */
    for (size_t i = 0; i < wapp->cfg.socketsCnt; i++) {
        const wapp_driver_t *s = &wapp->cfg.sockets[i];
        if (s->path[0] != '\0') {
            LOG_ERROR("sockets[%zu] '%s': name not allowed", i, s->name);
            ret += -EINVAL;
            continue;
        }
        char mount[CONFIG_WANTED_MAX_PATH_LEN];
        snprintf(mount, sizeof(mount), WANTED_NET_MOUNT_FMT, s->name);
        ret += WantedInstallDriver(ctx->vfs, wapp, "socket", mount, s->options);
    }

    /* mounts[]: file/backend drivers bound at an arbitrary absolute path. The
     * path is required, must be absolute, and must not fall under the fixed
     * /dev, /net or /proc namespaces. */
    for (size_t i = 0; i < wapp->cfg.mountsCnt; i++) {
        const wapp_driver_t *m = &wapp->cfg.mounts[i];
        if (m->path[0] != '/' || isReservedNamespace(m->path, "/dev") ||
            isReservedNamespace(m->path, "/net") ||
            isReservedNamespace(m->path, "/proc")) {
            LOG_ERROR("mounts[%zu] '%s': path '%s' must be absolute and "
                      "outside /dev, /net and /proc",
                      i, m->name, m->path);
            ret += -EINVAL;
            continue;
        }
        if (strcmp(m->name, "platform") == 0) {
            /* A `platform` mount is a bind mount: the host dir named by
             * `options` is bound as a WASI preopen at m->path. A malformed
             * options string fails the launch. */
            char hostPath[CONFIG_WANTED_MAX_PATH_LEN];
            bool readonly;
            int rc = parsePlatformMountOptions(m->options, hostPath,
                                               sizeof(hostPath), &readonly);
            if (rc < 0) {
                LOG_ERROR("mounts[%zu] '%s': malformed options '%s'", i,
                          m->name, m->options);
                ret += rc;
                continue;
            }
            const char *src = (hostPath[0] != '\0') ? hostPath : m->path;
            int host_fd = PlatformOpenStateDir(src, readonly);
            if (host_fd < 0) {
                LOG_ERROR("mounts[%zu] '%s': host dir '%s' unusable: %s", i,
                          m->name, src, strerror(-host_fd));
                /* A missing backing dir is a deployment error on a read-only
                 * mount and surfaced loudly. A read-write mount creates its
                 * dir, so an open failure there is environmental. */
                if (readonly)
                    ret += host_fd;
                continue;
            }
            rc = WasiCtxAddPreopen(wasiCtx, m->path, src, host_fd, readonly);
            if (rc < 0) {
                DEBUG_TRACE("WasiCtxAddPreopen(%s) failed: %d", m->path, rc);
            }
        } else if (strcmp(m->name, "volume") == 0) {
            /* A `volume` mount is an engine-managed named store: the engine
             * owns the host location and the wapp names only the volume, so
             * the store is portable across hosts. */
            char volName[CONFIG_WANTED_MAX_PATH_LEN];
            bool readonly, shared;
            int rc = parseVolumeMountOptions(
                m->options, volName, sizeof(volName), &readonly, &shared);
            if (rc < 0) {
                LOG_ERROR("mounts[%zu] '%s': malformed options '%s'", i,
                          m->name, m->options);
                ret += rc;
                continue;
            }
            char hostPath[CONFIG_WANTED_MAX_PATH_LEN];
            int n = shared
                        ? snprintf(hostPath, sizeof(hostPath),
                                   WANTED_VOLUME_SHARED_FMT,
                                   PlatformVolumeRoot(), volName)
                        : snprintf(hostPath, sizeof(hostPath),
                                   WANTED_VOLUME_PRIV_FMT, PlatformVolumeRoot(),
                                   wapp->name, volName);
            if (n < 0 || (size_t)n >= sizeof(hostPath)) {
                DEBUG_TRACE("mounts[%zu] '%s': volume path too long", i,
                            m->name);
                ret += -ENAMETOOLONG;
                continue;
            }
            /* The engine provisions the volume, so the backing dir is created
             * even for a read-only grant; `readonly` governs only the wapp's
             * access. A provisioning failure fails the launch. */
            int host_fd = PlatformOpenStateDir(hostPath, false);
            if (host_fd < 0) {
                LOG_ERROR("mounts[%zu] '%s': volume dir '%s' unusable: %s", i,
                          m->name, hostPath, strerror(-host_fd));
                ret += host_fd;
                continue;
            }
            rc = WasiCtxAddPreopen(wasiCtx, m->path, hostPath, host_fd,
                                   readonly);
            if (rc < 0) {
                DEBUG_TRACE("WasiCtxAddPreopen(%s) failed: %d", m->path, rc);
            }
#ifdef CONFIG_WANTED_VFS_LOGMOUNT
        } else if (strcmp(m->name, "log") == 0) {
            /* A `log` mount is the read-only directory view over the per-wapp
             * LogStore. `options` may carry `name=<wapp>` to scope the view to
             * one wapp; absent, it exposes every wapp's log. */
            vfs_driver_t *drv = VfsLogMountInit(wapp, m->options);
            if (drv == NULL) {
                LOG_ERROR("mounts[%zu] '%s': can't create log mount", i,
                          m->name);
                ret += -EINVAL;
                continue;
            }
            int rc = VfsMountDriver(ctx->vfs, m->path, drv);
            if (rc < 0) {
                DEBUG_TRACE("VfsMountDriver(%s) failed: %d", m->path, rc);
                if (drv->Destroy)
                    drv->Destroy(drv);
                ret += rc;
            }
#endif /* CONFIG_WANTED_VFS_LOGMOUNT */
        } else {
            ret += WantedInstallDriver(ctx->vfs, wapp, m->name, m->path,
                                       m->options);
        }
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
    uint32_t callArgv[1] = {(uint32_t)ctx->id};

    /* exit_code is authoritative only on a clean exit; default to the sentinel
     * so a trap (which never reaches proc_exit) stays distinguishable. */
    ctx->exit_code = WAPP_EXIT_CODE_NONE;

    if (wasm_runtime_call_wasm(ctx->wamr->exec_env, f, 1, callArgv)) {
        /* Returned without trapping — a clean exit. A WASI wapp leaves through
         * proc_exit; a bare wapp just returns. The recorded code is
         * authoritative (0 if proc_exit was never called). */
        ctx->exit_code = wasiCtx->exit_code;
    } else {
        const char *exc = wasm_runtime_get_exception(ctx->wamr->instance);
        /* WAMR prefixes the stored string ("Exception: proc_exit"), so match a
         * substring rather than the bare token. */
        if (exc && strstr(exc, "proc_exit") != NULL) {
            /* Clean exit via proc_exit (explicit exit() or return from _start);
             * proc_exit recorded the code on the wasi context. */
            DEBUG_TRACE("normal exit via proc_exit");
            ctx->exit_code = wasiCtx->exit_code;
        } else {
            /* Genuine trap: no WASI exit code exists. Leave the sentinel and
             * report failure so the slot transitions RUNNING -> FAILURE. */
            LOG_ERROR("wapp '%s' trapped: %s", wapp->name,
                      exc ? exc : "(no exception)");
            ret = -1;
            goto _freeVfs;
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
    PlatformExtramFree(ctx->wamr->wasm_bytes);
_freeWamr:
    WantedFree(ctx->wamr);
    /* The platform slot keeps pointing at this wapp_data_t; null the freed WAMR
     * handle so a later PlatformWappGetState (which samples per-wapp memory)
     * never dereferences a dangling instance. */
    ctx->wamr = NULL;
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
        PlatformExtramFree(ctx->wamr->wasm_bytes);
        WantedFree(ctx->wamr);
        ctx->wamr = NULL;

        DEBUG_TRACE("end");
    }

    /* Symmetric counterpart to wasm_runtime_init_thread_env(), run on every
     * exit path: it restores this thread's stack guard pages and frees its
     * signal alternate stack. Self-guards when no env was initialised. */
    wasm_runtime_destroy_thread_env();
}

void WantedWappTerminate(wapp_data_t *ctx) {
    /* Abort an instance's in-flight WASM execution: the worker's
     * wasm_runtime_call_wasm returns false and the thread unwinds through its
     * cleanup handler. Self-guards when the wapp has no live instance. */
    if (ctx == NULL || ctx->wamr == NULL || ctx->wamr->instance == NULL) {
        return;
    }
    wasm_runtime_terminate(ctx->wamr->instance);
}

/* Reload state. Single bytes are atomic on every target; `volatile` keeps the
 * run loop from caching a value the supervisor's thread changed. `reloadArmed`
 * is consumed at the next respawn; `pinBuiltin` survives a reload. */
static volatile uint8_t supervisorReloadArmed;
static volatile uint8_t supervisorPinBuiltin;

/* Image path to read: configured normally, compiled-in once rolled back. */
static const char *supervisorImagePath(const wantedConfig_t *cfg) {
    if (supervisorPinBuiltin)
        return SUPERVISOR_IMAGE_PATH;
    return (cfg && cfg->supervisorImagePath[0]) ? cfg->supervisorImagePath
                                                : SUPERVISOR_IMAGE_PATH;
}

/* A supervisor image named "registry:<name>[:<version>]" comes from the wapp
 * registry, which is what a control plane can install into. Anything else is a
 * platform image path.
 */
#define SUPERVISOR_REGISTRY_PREFIX "registry:"

static bool supervisorIsRegistryRef(const char *path) {
    const size_t prefix = sizeof(SUPERVISOR_REGISTRY_PREFIX) - 1;

    return strncmp(path, SUPERVISOR_REGISTRY_PREFIX, prefix) == 0 &&
           path[prefix] != '\0';
}

/* Load a supervisor image the registry holds. The reference is the one a
 * launch config's `image` uses, thus a supervisor is stored and named as every
 * other image is.
 */
static int loadSupervisorFromRegistry(const char *path, wapp_t *w) {
    const size_t prefix = sizeof(SUPERVISOR_REGISTRY_PREFIX) - 1;
    reg_entry_t e;
    const char *ref = path + prefix;
    const char *colon;

    memset(&e, 0, sizeof(e));
    colon = strchr(ref, ':');
    if (colon != NULL) {
        size_t nlen = (size_t)(colon - ref);

        if (nlen >= WAPP_MAX_NAME_LEN) {
            nlen = WAPP_MAX_NAME_LEN - 1;
        }

        memcpy(e.name, ref, nlen);
        e.name[nlen] = '\0';
        strncpy(e.version, colon + 1, WAPP_MAX_VERSION_LEN - 1);
        e.version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    } else {
        strncpy(e.name, ref, WAPP_MAX_NAME_LEN - 1);
        e.name[WAPP_MAX_NAME_LEN - 1] = '\0';
    }

    return PlatformRegistryWappLoad(&e, w);
}

/* Load the supervisor image, falling back to the compiled-in one. */
static int loadSupervisorImage(wapp_t *w, const wantedConfig_t *cfg) {
    const char *path = supervisorImagePath(cfg);

    int ret = supervisorIsRegistryRef(path)
                  ? loadSupervisorFromRegistry(path, w)
                  : PlatformWappLoad(path, w);

    if (ret < 0 && strcmp(path, SUPERVISOR_IMAGE_PATH) != 0) {
        DEBUG_TRACE("staged supervisor %s failed (%d); using built-in %s", path,
                    ret, SUPERVISOR_IMAGE_PATH);
        path = SUPERVISOR_IMAGE_PATH;
        ret = PlatformWappLoad(path, w);
    }
    if (ret < 0) {
        DEBUG_TRACE("failed to load supervisor image from %s: %d", path, ret);
        /* Drop the layer bookkeeping so the struct keeps its invariant: a
         * valid wapp has layer_cnt >= 1. PlatformWappUnload takes a const
         * wapp_t, so a failed reload would otherwise leave layers[0] freed. */
        memset((void *)w->layers, 0, sizeof(w->layers));
        memset(w->layer_lens, 0, sizeof(w->layer_lens));
        w->layer_cnt = 0;
    }

    return ret;
}

int WantedSupervisorReload(void) {
    supervisorReloadArmed = 1;
    return 0;
}

int WantedSupervisorRollback(void) {
    /* Already on the built-in image — nothing to fall back to. */
    if (supervisorPinBuiltin)
        return -1;

    supervisorPinBuiltin = 1;
    supervisorReloadArmed = 1;
    return 0;
}

/* Consecutive observations a supervisor must be seen running before an exit
 * counts as ordinary. A platform run loop polls at 1 Hz, so this is seconds:
 * clear of a normal startup, far below any real working lifetime. */
#define SUPERVISOR_HEALTHY_TICKS 10

/* True while the running image is a staged one, thus a rollback has somewhere
 * to go. */
static bool supervisorStaged(void) {
    const wantedConfig_t *cfg = WantedGetConfig();

    if (supervisorPinBuiltin)
        return false;
    return cfg != NULL && cfg->supervisorImagePath[0] != '\0' &&
           strcmp(cfg->supervisorImagePath, SUPERVISOR_IMAGE_PATH) != 0;
}

supervisorHealth_t WantedSupervisorObserve(bool running, bool failed,
                                           bool exited) {
    static int healthyTicks;
    static int failures;

    if (running) {
        if (healthyTicks < SUPERVISOR_HEALTHY_TICKS)
            healthyTicks++;
        /* Only a supervisor that ran its full healthy span clears the count.
         * Clearing on any running tick would let an image that dies just short
         * of that span restart forever, each attempt erasing the last. */
        if (healthyTicks >= SUPERVISOR_HEALTHY_TICKS)
            failures = 0;
        return SUPERVISOR_HEALTHY;
    }

    /* A launch that failed outright, or a staged image that reached its entry
     * point and left again at once, which is what an incompatible supervisor
     * looks like. Judged only while a staged image runs; see the docs. */
    bool suspect = failed || (exited && supervisorStaged() &&
                              healthyTicks < SUPERVISOR_HEALTHY_TICKS);
    healthyTicks = 0;

    if (!suspect) {
        failures = 0;
        return SUPERVISOR_RESPAWN;
    }
    if (++failures < MAX_SUPERVISOR_LAUNCH_FAILURES)
        return SUPERVISOR_RESPAWN;

    failures = 0;
    return WantedSupervisorRollback() == 0 ? SUPERVISOR_ROLLED_BACK
                                           : SUPERVISOR_UNRECOVERABLE;
}

wapp_t *WantedGetCurrentSupervisor(void) {
    int ret = 0;
    static wapp_t *w = NULL;

    if (NULL != w) {
        /* An armed reload re-reads the image in place. The old mapping is
         * dropped only here, while no supervisor is running. */
        if (supervisorReloadArmed) {
            supervisorReloadArmed = 0;
            PlatformWappUnload(w);
            loadSupervisorImage(w, WantedGetConfig());
        }
        return w;
    }

    w = WantedMalloc(sizeof(wapp_t));
    /* The supervisor is loaded directly, so the loader never stamps its image
     * and version fields. Zero them so the control-plane string nodes read
     * empty rather than uninitialised memory. */
    memset(w, 0, sizeof(wapp_t));
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

    loadSupervisorImage(w, cfg);

    return w;
}

int WantedStart(const char *cfg, size_t cfgLen) {
    int ret;
    wapp_t *app;

    ret = WantedParseConfig(cfg, cfgLen);
    DEBUG_TRACE("WantedParseConfig -> %d", ret);
    if (ret < 0)
        return ret;

    ret = ensureWamrInit();
    DEBUG_TRACE("ensureWamrInit -> %d", ret);
    if (ret < 0)
        return -1;

    app = WantedGetCurrentSupervisor();
    DEBUG_TRACE("WantedGetCurrentSupervisor -> %p", (void *)app);

    ret = PlatformWappStart(app);
    DEBUG_TRACE("PlatformWappStart -> %d", ret);
    if (ret < 0)
        return ret;

    PlatformWappLoop();
    DEBUG_TRACE("PlatformWappLoop returned");

    return 0;
}
