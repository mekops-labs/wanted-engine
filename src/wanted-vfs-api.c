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
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

#include <tiny-json.h>

static wantedConfig_t currentConfig;

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

const char *statusToString(status_t state) {
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

static int ParseConfig(const char *buf, size_t len, wantedConfig_t *out) {
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
    return ParseConfig(buf, bufLen, &currentConfig);
}

const wantedConfig_t *WantedGetConfig(void) { return &currentConfig; }

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

int WantedRenderRegistryDescriptor(const reg_entry_t *entry, uint8_t *buf,
                                   size_t bufLen) {
    if (entry == NULL || buf == NULL)
        return -EINVAL;

    /* Inspecting a registry entry returns a small descriptor synthesized from
     * the entry itself (name/version/size). */
    int n = snprintf((char *)buf, bufLen,
                     "{\"name\":\"%s\",\"version\":\"%s\",\"size\":%zu}",
                     entry->name, entry->version, entry->size);
    if (n < 0)
        return -EIO;
    return n < (int)bufLen ? n : (int)bufLen;
}

/* Table adaptor: a config-named `platform` driver — a console backing or a
 * /dev singleton — is always read-write. The read-only bind mount is bound via
 * WasiCtxAddPreopen (the mounts[] path), not resolved through this table. */
static vfs_driver_t *PlatformFsInitRW(const wapp_t *wapp, const char *options) {
    return VfsPlatformFsInit(wapp, options, false);
}

/* Global driver table — single registry used by WantedInstallDriver to resolve
 * a config driver name into an init callback. */
static const vfs_driver_table_t global_driver_table[] = {
    {"null", VfsNullInit},
    {"log", VfsLogInit},
    {"9p", Vfs9PInit},
    {"config", VfsConfigInit},
    {"platform", PlatformFsInitRW},
    {"socket", VfsSocketInit},
    {"wanted", VfsWantedInit},
    {NULL, NULL},
};

/* Route a resolved driver to its mount target:
 *   /dev/<x>  → DevFs registration table   (device singletons)
 *   /net/<x>  → NetFs registration table   (sockets)
 *   <stdio>   → STREAM slot in the typed-FD table (console)
 *   /<abs>    → general single-driver mount (file/backend drivers)
 * A malformed path (relative, or an unknown <stdio> token) is rejected and the
 * driver destroyed: a misconfigured launch config fails loudly at install time
 * rather than silently at first open. */
static int InstallTo(struct vfs_ctx_t *c, const char *path,
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
    DEBUG_TRACE("InstallTo: unrouted path '%s', dropping driver", path);
    if (drv->Destroy)
        drv->Destroy((vfs_driver_t *)drv);
    return -EINVAL;
}

int WantedInstallDriver(struct vfs_ctx_t *c, const wapp_t *w, const char *name,
                        const char *path, const char *options) {
    int ret = 0;
    int i = 0;
    const vfs_driver_t *drv = NULL;

    if (c == NULL || w == NULL || name == NULL || path == NULL) {
        return -EINVAL;
    }

    while ((global_driver_table[i].name != NULL) &&
           (global_driver_table[i].init != NULL)) {
        if (memcmp(global_driver_table[i].name, name,
                   strlen(global_driver_table[i].name)) == 0) {
            drv = global_driver_table[i].init(w, options);
            if (NULL == drv) {
                DEBUG_TRACE("can't load %s driver (%d)", name, ret);
                return -EINVAL;
            }
        }
        i++;
    }

    if (NULL != drv) {
        ret = InstallTo(c, path, drv);
    } else {
        DEBUG_TRACE("can't load %s driver, not found", name);
        return -EINVAL;
    }

    return ret;
}

/* Parse one launch-config resource section ("drivers"/"mounts"/"sockets") into
 * `arr`. Each entry reads "name", "path", and the section's options field
 * (`optKey`, "options" or "address"). A field a section forbids is still read
 * here so install-time validation can reject it loudly. */
static void ParseResourceArray(json_t const *params, const char *section,
                               const char *optKey, wapp_driver_t *arr,
                               size_t *cnt) {
    json_t const *a = json_getProperty(params, section);
    size_t i = 0;

    if (a && JSON_ARRAY == json_getType(a)) {
        for (json_t const *e = json_getChild(a); e && i < MAX_DRIVERS_CNT;
             e = json_getSibling(e)) {
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
static void ParseWappParams(json_t const *params, wapp_config_t *cfg) {
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
    ParseResourceArray(params, "drivers", "options", cfg->drivers,
                       &cfg->driversCnt);
    ParseResourceArray(params, "mounts", "options", cfg->mounts,
                       &cfg->mountsCnt);
    ParseResourceArray(params, "sockets", "address", cfg->sockets,
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
            if (!v || vlen >= WAPP_MAX_ARG_LEN)
                continue;
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
            if (!v || vlen >= WAPP_MAX_ENV_LEN)
                continue;
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

    ParseWappParams(params, cfg);

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
    ParseWappParams(json, cfg);

    return 0;
}
