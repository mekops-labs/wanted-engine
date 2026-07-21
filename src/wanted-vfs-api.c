/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug_trace.h>
#include <platform.h>
#include <vfs-devfs.h>
#include <vfs-drivers.h>
#include <vfs-netfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted-vfs-api.h>
#include <wanted_log.h>
#include <wanted_malloc.h>

#include <tiny-json.h>

/* The engine's parsed bootstrap config. Heap-allocated on first parse rather
 * than living in static .bss: wantedConfig_t embeds a wapp_config_t (the
 * supervisor's launch config, with its driver/mount/socket slot tables), which
 * is large on constrained targets; the engine heap may extend into PSRAM. */
static wantedConfig_t *currentConfig = NULL;

/* Bounded copy of a (possibly NULL) JSON string value into a fixed-size config
 * field. Truncates rather than overflowing; always NUL-terminates. */
static void copyField(char *dst, size_t dstsz, const char *src) {
    snprintf(dst, dstsz, "%s", src ? src : "");
}

/* Engine clock-quality byte. Defaults to UNCALIBRATED so wapps that consult
 * it before any platform timing subsystem has come up get the safe answer.
 * The byte is single-aligned so reads/writes are naturally atomic on every
 * relevant target; `volatile` prevents the compiler from caching a stale
 * value across the boundary between updater and reader threads. */
static volatile uint8_t clockQuality = WANTED_CLOCK_UNCALIBRATED;

void WantedSetClockQuality(uint8_t q) {
    if (q <= WANTED_CLOCK_UNCALIBRATED)
        clockQuality = q;
}

uint8_t WantedGetClockQuality(void) { return clockQuality; }

int WantedProcReadClockQuality(vfs_ctx_t c, void *buf, size_t bufLen) {
    (void)c;
    if (!buf || bufLen < 1)
        return -EINVAL;
    *(uint8_t *)buf = clockQuality;
    return 1;
}

const char *StatusToString(status_t state) {
    switch (state) {
    case NOT_STARTED:
        return "not_started";
    case CREATED:
        return "created";
    case STARTING:
        return "starting";
    case RUNNING:
        return "running";
    case EXITED:
        return "exited";
    case FAILURE:
        return "failure";
    default:
        return "unknown";
    }
}

static int parseConfig(const char *buf, size_t len, wantedConfig_t *out) {
    json_t m[100];
    char b[len];

    if (NULL == out || NULL == buf) {
        return -EINVAL;
    }

    memcpy(b, buf, len);
    memset(out, 0, sizeof(wantedConfig_t));

    json_t const *json = json_create(b, m, sizeof m / sizeof *m);
    if (!json || JSON_OBJ != json_getType(json)) {
        DEBUG_TRACE("can't initialize json parser");
        return -EINVAL;
    }

    json_t const *system = json_getProperty(json, "system");
    if (!system || JSON_OBJ != json_getType(system)) {
        DEBUG_TRACE(".system property not found in json");
        return -EINVAL;
    }

    json_t const *priv = json_getProperty(system, "privileged");
    if (priv && JSON_BOOLEAN == json_getType(priv))
        out->privileged = json_getBoolean(priv);

    json_t const *supervisor = json_getProperty(json, "supervisor");
    if (supervisor && JSON_OBJ == json_getType(supervisor)) {
        const char *imgPath = json_getPropertyValue(supervisor, "imagePath");
        if (imgPath)
            strncpy(out->supervisorImagePath, imgPath,
                    sizeof(out->supervisorImagePath) - 1);

        if (WantedParseCtrlAction(supervisor, NULL, NULL,
                                  &out->supervisorCfg) == 0) {
            out->supervisorCfg.valid = true;
        } else {
            DEBUG_TRACE(".supervisor property parsing error");
        }
    } else {
        DEBUG_TRACE(".supervisor property not found in json");
    }

    return 0;
}

int WantedParseConfig(const char *buf, size_t bufLen) {
    if (currentConfig == NULL) {
        currentConfig = WantedMalloc(sizeof(wantedConfig_t));
        if (currentConfig == NULL)
            return -ENOMEM;
    }
    return parseConfig(buf, bufLen, currentConfig);
}

/* Always returns a usable config: if nothing has been parsed yet, a zeroed one
 * is allocated so callers (e.g. the privileged-flag check) never see NULL —
 * matching the previous always-present static. */
const wantedConfig_t *WantedGetConfig(void) {
    if (currentConfig == NULL) {
        currentConfig = WantedMalloc(sizeof(wantedConfig_t));
        if (currentConfig != NULL)
            memset(currentConfig, 0, sizeof(wantedConfig_t));
    }
    return currentConfig;
}

int WantedWriteRegistry(bool *cont, const char *ref, const uint8_t *buf,
                        size_t bufLen) {
    if (buf == NULL)
        return -EINVAL;

    /* The ref ("<name>:<version>") names the install target; it is consumed at
     * START_WRITE and ignored thereafter. */
    if (*cont == false) {
        *cont = true;
        return PlatformRegistryWrite(START_WRITE, ref, buf, bufLen);
    }

    return PlatformRegistryWrite(CONTINUE_WRITE, ref, buf, bufLen);
}

int WantedRegistryRemove(const reg_entry_t *entry) {
    if (entry == NULL)
        return -EINVAL;

    return PlatformRegistryRemove(entry);
}

int WantedCloseRegistry(void) {
    return PlatformRegistryWrite(FINISH_WRITE, NULL, NULL, 0);
}

/* Read a uLEB128-encoded u32 at *p (bounded by end), advancing *p. Returns 0,
 * or -EINVAL on truncation / a >5-byte encoding. */
static int ulebU32(const uint8_t **p, const uint8_t *end, uint32_t *out) {
    uint32_t result = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 32)
            result |= (uint32_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = result;
            return 0;
        }
        shift += 7;
        if (shift >= 35)
            return -EINVAL; /* more than 5 bytes — not a valid u32 */
    }
    return -EINVAL; /* ran off the end mid-encoding */
}

int WantedWasmMemoryProfile(const uint8_t *buf, size_t len, uint32_t *init,
                            bool *has_max, uint32_t *max) {
    static const uint8_t MAGIC[8] = {0x00, 0x61, 0x73, 0x6d,
                                     0x01, 0x00, 0x00, 0x00};
    if (buf == NULL || init == NULL || has_max == NULL || max == NULL)
        return -EINVAL;
    if (len < sizeof(MAGIC) || memcmp(buf, MAGIC, sizeof(MAGIC)) != 0)
        return -EINVAL;

    const uint8_t *p = buf + sizeof(MAGIC);
    const uint8_t *end = buf + len;

    /* Sections are emitted in ascending id order; the memory section is id 5.
     * Walk section headers (id byte + uLEB length), entering id 5 and skipping
     * the rest. A section whose declared length runs past the buffer means the
     * caller's window stopped short of the memory section — report "absent"
     * (ENOENT) so the descriptor omits the fields rather than guessing. */
    while (p < end) {
        uint8_t id = *p++;
        uint32_t secLen;
        if (ulebU32(&p, end, &secLen) < 0)
            return -EINVAL;
        if (secLen > (uint32_t)(end - p))
            return id < 5 ? -ENOENT : -EINVAL;
        const uint8_t *secEnd = p + secLen;

        if (id == 5) {
            uint32_t count;
            if (ulebU32(&p, secEnd, &count) < 0)
                return -EINVAL;
            if (count == 0)
                return -ENOENT; /* declared, but no memory */
            if (p >= secEnd)
                return -EINVAL;
            uint8_t flags = *p++;
            uint32_t mn;
            if (ulebU32(&p, secEnd, &mn) < 0)
                return -EINVAL;
            *init = mn;
            *has_max = (flags & 0x01) != 0;
            *max = 0;
            if (*has_max && ulebU32(&p, secEnd, max) < 0)
                return -EINVAL;
            return 0;
        }
        if (id > 5)
            break; /* past where the memory section would be */
        p = secEnd;
    }
    return -ENOENT; /* no memory section (e.g. an imported memory) */
}

/* Leading bytes of an image to peek for the wasm memory section. The section
 * precedes the (bulky) code section, so a small window reaches it for real
 * wapps; a too-short window just yields no memory metadata. */
#define REG_IMAGE_PEEK 8192
#define TAR_BLOCK 512

/* Append the image's declared linear-memory profile to the descriptor JSON at
 * *n (which currently ends just before the closing brace). Best effort: on any
 * failure the descriptor is left unchanged, so the fields are simply absent. */
static void appendMemoryProfile(const reg_entry_t *entry, char *buf, int *n,
                                size_t bufLen) {
    uint8_t *peek = WantedMalloc(REG_IMAGE_PEEK);
    if (peek == NULL)
        return;

    int got = PlatformRegistryReadImage(entry, peek, REG_IMAGE_PEEK);
    /* The .wapp is a ustar archive whose first member is app.wasm; its content
     * begins at the 512-byte block boundary after the header. */
    if (got > TAR_BLOCK && memcmp(peek, "app.wasm", sizeof("app.wasm")) == 0) {
        uint32_t init = 0, max = 0;
        bool has_max = false;
        if (WantedWasmMemoryProfile(peek + TAR_BLOCK, (size_t)got - TAR_BLOCK,
                                    &init, &has_max, &max) == 0) {
            bool can_grow = !has_max || max > init;
            int w = snprintf(buf + *n, bufLen - (size_t)*n,
                             ",\"init_pages\":%u,\"can_grow\":%s",
                             (unsigned)init, can_grow ? "true" : "false");
            if (w > 0 && *n + w < (int)bufLen)
                *n += w;

            if (has_max)
                w = snprintf(buf + *n, bufLen - (size_t)*n, ",\"max_pages\":%u",
                             (unsigned)max);
            else
                w = snprintf(buf + *n, bufLen - (size_t)*n,
                             ",\"max_pages\":null");
            if (w > 0 && *n + w < (int)bufLen)
                *n += w;

#if CONFIG_WANTED_WASM_MAX_MEMORY_PAGES > 0
            /* The build caps per-wapp linear memory; flag an image whose
             * declared initial memory already exceeds the cap — it would be
             * refused at load. */
            w = snprintf(buf + *n, bufLen - (size_t)*n, ",\"over_cap\":%s",
                         init > CONFIG_WANTED_WASM_MAX_MEMORY_PAGES ? "true"
                                                                    : "false");
            if (w > 0 && *n + w < (int)bufLen)
                *n += w;
#endif
        }
    }

    WantedFree(peek);
}

int WantedRenderRegistryDescriptor(const reg_entry_t *entry, uint8_t *buf,
                                   size_t bufLen) {
    if (entry == NULL || buf == NULL)
        return -EINVAL;

    /* Inspecting a registry entry returns a small descriptor synthesized from
     * the entry (name/version/size), plus the image's declared linear-memory
     * profile when it can be read from the image header. */
    int n = snprintf((char *)buf, bufLen,
                     "{\"name\":\"%s\",\"version\":\"%s\",\"size\":%zu",
                     entry->name, entry->version, entry->size);
    if (n < 0)
        return -EIO;
    if (n >= (int)bufLen)
        return (int)bufLen;

    appendMemoryProfile(entry, (char *)buf, &n, bufLen);

    int w = snprintf((char *)buf + n, bufLen - (size_t)n, "}");
    if (w > 0 && n + w < (int)bufLen)
        n += w;
    return n < (int)bufLen ? n : (int)bufLen;
}

/* Table adaptor: a config-named `platform` driver — a console backing or a
 * /dev singleton — is always read-write. The read-only bind mount is bound via
 * WasiCtxAddPreopen (the mounts[] path), not resolved through this table. */
static vfs_driver_t *platformFsInitRW(const wapp_t *wapp, const char *options) {
    return VfsPlatformFsInit(wapp, options, false);
}

/* Core driver table — the platform-agnostic drivers, identical on every target
 * Platform-specific drivers a target may
 * lack (gpio, wifi, ...) come from PlatformDriverTable() instead. Core names
 * are reserved: this table is searched first, so no other table can shadow a
 * security-relevant driver such as `wanted`. */
static const vfs_driver_table_t core_driver_table[] = {
    {"null", VfsNullInit},
    {"log", VfsLogInit},
    {"platform", platformFsInitRW},
#ifdef CONFIG_WANTED_VFS_9P
    {"9p", Vfs9PInit},
#endif
#ifdef CONFIG_WANTED_VFS_CONFIG
    {"config", VfsConfigInit},
#endif
#ifdef CONFIG_WANTED_VFS_SOCKET
    {"socket", VfsSocketInit},
#endif
#ifdef CONFIG_WANTED_VFS_SHA256
    {"sha256", VfsSha256Init},
#endif
#ifdef CONFIG_WANTED_VFS_ED25519
    {"ed25519", VfsEd25519Init},
#endif
#ifdef CONFIG_WANTED_VFS_INFLATE
    {"inflate", VfsInflateInit},
#endif
#ifdef CONFIG_WANTED_VFS_WANTED
    {"wanted", VfsWantedInit},
#endif
    {NULL, NULL},
};

/* The tables a driver name resolves against, in search order: core names are
 * reserved, the platform contributes what the target implements, and an
 * out-of-tree tree may append its own. Lowest priority last, so neither a
 * platform nor an extra table can shadow a core driver. */
static const vfs_driver_table_t *driverTables(size_t i) {
    switch (i) {
    case 0:
        return core_driver_table;
    case 1:
        return PlatformDriverTable();
    case 2:
        return ExtraDriverTable();
    default:
        return NULL;
    }
}

#define DRIVER_TABLE_CNT 3

/* Resolve a config driver name to its init callback by exact match. Returns
 * NULL when no table offers the name — the driver is not available on this
 * build. */
static VfsInitFunction_t resolveDriver(const char *name) {
    for (size_t t = 0; t < DRIVER_TABLE_CNT; t++) {
        const vfs_driver_table_t *tab = driverTables(t);
        for (int i = 0; tab != NULL && tab[i].name != NULL; i++) {
            if (strcmp(tab[i].name, name) == 0)
                return tab[i].init;
        }
    }

    return NULL;
}

int WantedListDrivers(char *buf, size_t bufLen) {
    if (buf == NULL || bufLen == 0)
        return -EINVAL;

    int w = 0;

    for (size_t t = 0; t < DRIVER_TABLE_CNT; t++) {
        const vfs_driver_table_t *tab = driverTables(t);
        for (int i = 0; tab != NULL && tab[i].name != NULL; i++) {
            int n = snprintf(buf + w, bufLen - (size_t)w, "%s%s",
                             w > 0 ? " " : "", tab[i].name);
            if (n < 0)
                return -EIO;
            w += n;
            if ((size_t)w >= bufLen)
                return (int)bufLen;
        }
    }

    return w;
}

/* Route a resolved driver to its mount target:
 *   /dev/<x>  → DevFs registration table   (device singletons)
 *   /net/<x>  → NetFs registration table   (sockets)
 *   <stdio>   → STREAM slot in the typed-FD table (console)
 *   /<abs>    → general single-driver mount (file/backend drivers)
 * A malformed path (relative, or an unknown <stdio> token) is rejected and the
 * driver destroyed: a misconfigured launch config fails loudly at install time
 * rather than silently at first open. */
static int installTo(struct vfs_ctx_t *c, const char *path,
                     const vfs_driver_t *drv) {
    if (strncmp(path, "/dev/", 5) == 0)
        return DevFs_Register(c, path + 5, drv);
    if (strncmp(path, "/net/", 5) == 0)
        return NetFs_Register(c, path + 5, drv);
    if (path[0] == '<')
        return VfsRegister(c, path, drv);
    if (path[0] == '/') {
        int r = VfsMountDriver(c, path, drv);
        if (r < 0 && drv->Destroy)
            drv->Destroy((vfs_driver_t *)drv);
        return r;
    }
    DEBUG_TRACE("installTo: unrouted path '%s', dropping driver", path);
    if (drv->Destroy)
        drv->Destroy((vfs_driver_t *)drv);
    return -EINVAL;
}

int WantedInstallDriver(struct vfs_ctx_t *c, const wapp_t *w, const char *name,
                        const char *path, const char *options) {
    const vfs_driver_t *drv = NULL;

    if (c == NULL || w == NULL || name == NULL || path == NULL) {
        return -EINVAL;
    }

    VfsInitFunction_t init = resolveDriver(name);
    if (init == NULL) {
        DEBUG_TRACE("driver '%s' not available on this platform", name);
        return -ENODEV;
    }

    drv = init(w, options);
    if (NULL == drv) {
        DEBUG_TRACE("can't load %s driver", name);
        return -EINVAL;
    }

    return installTo(c, path, drv);
}

/* Parse one launch-config resource section ("drivers"/"mounts"/"sockets") into
 * `arr`. Each entry reads "name", "path", and the section's options field
 * (`optKey`, "options" or "address"). A field a section forbids is still read
 * here so install-time validation can reject it loudly. */
static void parseResourceArray(json_t const *params, const char *section,
                               const char *optKey, wapp_driver_t *arr,
                               size_t *cnt) {
    json_t const *a = json_getProperty(params, section);
    size_t i = 0;

    if (a && JSON_ARRAY == json_getType(a)) {
        for (json_t const *e = json_getChild(a);
             e && i < CONFIG_WANTED_MAX_DRIVERS_CNT; e = json_getSibling(e)) {
            if (JSON_OBJ != json_getType(e))
                continue;
            const char *name = json_getPropertyValue(e, "name");
            const char *path = json_getPropertyValue(e, "path");
            const char *opt = json_getPropertyValue(e, optKey);
            copyField(arr[i].name, sizeof(arr[i].name), name);
            copyField(arr[i].path, sizeof(arr[i].path), path);
            copyField(arr[i].options, sizeof(arr[i].options), opt);
            i++;
        }
    }
    *cnt = i;
}

/* Parse the launch-config body — console redirections plus the drivers/mounts/
 * sockets resource sections — out of `params`. Shared by the {action,params}
 * bootstrap envelope (WantedParseCtrlAction) and the per-wapp config node
 * (WantedParseWappConfigJson), where the object passed in *is* the config.
 * Wapp identity is not read here — for the config node it travels in the
 * path. */
static void parseWappParams(json_t const *params, wapp_config_t *cfg) {
    /* image: the registry image this instance runs, as a reference
     * "<name>[:<tag>]". Optional — when omitted the launch path defaults it to
     * the instance name, so a single-instance wapp needs no config change. A
     * bare name resolves to the first match; a pinned tag resolves exactly.
     * Lets N instances share one image. */
    const char *image = json_getPropertyValue(params, "image");
    if (image != NULL) {
        strncpy(cfg->image, image, WAPP_MAX_IMAGE_REF_LEN - 1);
        cfg->image[WAPP_MAX_IMAGE_REF_LEN - 1] = '\0';
    }

    json_t const *console = json_getProperty(params, "console");
    if (console && JSON_OBJ == json_getType(console)) {
        json_t const *in = json_getProperty(console, "in");
        if (in && JSON_OBJ == json_getType(in)) {
            copyField(cfg->console[0].name, sizeof(cfg->console[0].name),
                      json_getPropertyValue(in, "name"));
            copyField(cfg->console[0].options, sizeof(cfg->console[0].options),
                      json_getPropertyValue(in, "options"));
        }

        json_t const *out = json_getProperty(console, "out");
        if (out && JSON_OBJ == json_getType(out)) {
            copyField(cfg->console[1].name, sizeof(cfg->console[1].name),
                      json_getPropertyValue(out, "name"));
            copyField(cfg->console[1].options, sizeof(cfg->console[1].options),
                      json_getPropertyValue(out, "options"));
        }

        json_t const *err = json_getProperty(console, "err");
        if (err && JSON_OBJ == json_getType(err)) {
            copyField(cfg->console[2].name, sizeof(cfg->console[2].name),
                      json_getPropertyValue(err, "name"));
            copyField(cfg->console[2].options, sizeof(cfg->console[2].options),
                      json_getPropertyValue(err, "options"));
        }
    }

    /* The launch config addresses resources through three purpose-specific
     * sections, each parsed into its own array:
     *   - drivers[] — device singletons (`name`), mounted at "/dev/<name>".
     *   - mounts[]  — file/backend drivers bound at an arbitrary `path`.
     *   - sockets[] — connections at "/net/<name>"; transport in "address".
     * Per-section validation (forbidden fields, required path) happens at
     * install time; here we only read the fields each section may carry. */
    parseResourceArray(params, "drivers", "options", cfg->drivers,
                       &cfg->driversCnt);
    parseResourceArray(params, "mounts", "options", cfg->mounts,
                       &cfg->mountsCnt);
    parseResourceArray(params, "sockets", "address", cfg->sockets,
                       &cfg->socketsCnt);

    /* args[]: optional command-line arguments, occupying argv[1..]. argv[0] is
     * the wapp name, set by the engine at launch. */
    json_t const *args = json_getProperty(params, "args");
    if (args && JSON_ARRAY == json_getType(args)) {
        json_t const *a;
        int ai = 0;
        for (a = json_getChild(args); a && ai < WAPP_MAX_ARGS;
             a = json_getSibling(a)) {
            if (JSON_TEXT != json_getType(a))
                continue;
            const char *v = json_getValue(a);
            size_t vlen = v ? strnlen(v, WAPP_MAX_ARG_LEN) : WAPP_MAX_ARG_LEN;
            if (!v || vlen >= WAPP_MAX_ARG_LEN) {
                LOG_ERROR("args[%d] dropped: longer than %d bytes", ai,
                          WAPP_MAX_ARG_LEN - 1);
                continue;
            }
            memcpy(cfg->args[ai], v, vlen);
            cfg->args[ai][vlen] = '\0';
            ai++;
        }
        cfg->argsCnt = (size_t)ai;
    }

    /* envs[]: optional environment, each a POSIX "KEY=VALUE" string. */
    json_t const *envs = json_getProperty(params, "envs");
    if (envs && JSON_ARRAY == json_getType(envs)) {
        json_t const *e;
        int ei = 0;
        for (e = json_getChild(envs); e && ei < WAPP_MAX_ENVS;
             e = json_getSibling(e)) {
            if (JSON_TEXT != json_getType(e))
                continue;
            const char *v = json_getValue(e);
            size_t vlen = v ? strnlen(v, WAPP_MAX_ENV_LEN) : WAPP_MAX_ENV_LEN;
            if (!v || vlen >= WAPP_MAX_ENV_LEN) {
                LOG_ERROR("envs[%d] dropped: longer than %d bytes", ei,
                          WAPP_MAX_ENV_LEN - 1);
                continue;
            }
            memcpy(cfg->envs[ei], v, vlen);
            cfg->envs[ei][vlen] = '\0';
            ei++;
        }
        cfg->envsCnt = (size_t)ei;
    }
}

int WantedParseCtrlAction(json_t const *json, char *wappName,
                          wapp_action_t *act, wapp_config_t *cfg) {
    if (NULL == cfg)
        return -EINVAL;

    if (NULL != act) {
        json_t const *action = json_getProperty(json, "action");
        if (!action || JSON_TEXT != json_getType(action)) {
            DEBUG_TRACE(".action property not found in json");
            return -EINVAL;
        }
        if (strcmp("start", json_getValue(action)) == 0) {
            *act = WAPP_START;
        } else if (strcmp("stop", json_getValue(action)) == 0) {
            *act = WAPP_STOP;
        } else {
            DEBUG_TRACE(".action property has wrong value");
            return -EINVAL;
        }
    }

    json_t const *params = json_getProperty(json, "params");
    if (!params || JSON_OBJ != json_getType(params)) {
        DEBUG_TRACE(".params property not found in json");
        return -EINVAL;
    }

    if (wappName != NULL) {
        json_t const *name = json_getProperty(params, "name");
        if (!name || JSON_TEXT != json_getType(name)) {
            DEBUG_TRACE(".params.name property not found in json");
            return -EINVAL;
        }
        copyField(wappName, WAPP_MAX_NAME_LEN, json_getValue(name));
    }

    parseWappParams(params, cfg);

    return 0;
}

int WantedParseCtrlActionJson(const char *buf, size_t bufLen, char *wappName,
                              wapp_action_t *act, wapp_config_t *cfg) {
    if (NULL == buf || NULL == cfg)
        return -EINVAL;
    /* Parses the compiled-in supervisor bootstrap config, which sits well
     * under the cap. */
    if (bufLen >= WANTED_CTRL_JSON_MAX)
        return -EMSGSIZE;

    json_t m[100];
    char b[WANTED_CTRL_JSON_MAX];

    memcpy(b, buf, bufLen);
    b[bufLen] = '\0';
    memset(cfg, 0, sizeof(wapp_config_t));

    json_t const *json = json_create(b, m, sizeof m / sizeof *m);
    if (!json || JSON_OBJ != json_getType(json)) {
        DEBUG_TRACE("can't initialize json parser");
        return -EINVAL;
    }

    return WantedParseCtrlAction(json, wappName, act, cfg);
}

int WantedParseWappConfigJson(const char *buf, size_t bufLen,
                              wapp_config_t *cfg) {
    if (NULL == buf || NULL == cfg)
        return -EINVAL;
    if (bufLen >= WANTED_CTRL_JSON_MAX)
        return -EMSGSIZE;

    json_t m[100];
    char b[WANTED_CTRL_JSON_MAX];

    memcpy(b, buf, bufLen);
    b[bufLen] = '\0';
    memset(cfg, 0, sizeof(wapp_config_t));

    json_t const *json = json_create(b, m, sizeof m / sizeof *m);
    if (!json || JSON_OBJ != json_getType(json)) {
        DEBUG_TRACE("can't initialize json parser");
        return -EINVAL;
    }

    /* The decomposed config node carries the bare launch-config body — the
     * object itself plays the role the legacy `params` block did. */
    parseWappParams(json, cfg);

    return 0;
}
