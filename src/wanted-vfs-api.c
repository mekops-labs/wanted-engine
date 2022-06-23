#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <debug_trace.h>
#include <wanted-vfs-api.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <wanted-api.h>

#include <json-maker/json-maker.h>
#include <tiny-json.h>

static wantedConfig_t currentConfig;


static size_t ConfigToJson(const wantedConfig_t *cfg, uint8_t *buf, size_t bufLen)
{
    char *p = (char *)buf;
    size_t left = bufLen;

    p = json_objOpen(p, NULL, &left);
    p = json_arrOpen(p, "wapps", &left);
    for (int i = 0; i < cfg->nWapps; i++) {
        p = json_str(p, NULL, cfg->wappsToRun[i], &left);
    }
    p = json_arrClose(p, &left);
    p = json_objClose(p, &left);
    p = json_end(p, &left);

    return bufLen - left;
}

static size_t RegistryToJson(const reg_entry_t *reg, size_t regLen, uint8_t *buf, size_t bufLen)
{
    char *p = (char *)buf;
    size_t left = bufLen;

    p = json_objOpen(p, NULL, &left);
    p = json_arrOpen(p, "wapps", &left);
    for (int i = 0; i < regLen; i++) {
        p = json_objOpen(p, NULL, &left);
        p = json_str(p, "name", reg[i].name, &left);
        p = json_str(p, "version", reg[i].version, &left);
        p = json_int(p, "size", reg[i].size, &left);
        p = json_objClose(p, &left);
    }
    p = json_arrClose(p, &left);
    p = json_objClose(p, &left);
    p = json_end(p, &left);

    return bufLen - left;
}

const char *statusToString(status_t state)
{
    switch(state) {
    case NOT_STARTED: return "NOT_STARTED";
    case STARTING:    return "STARTING";
    case RUNNING:     return "RUNNING";
    case EXITED:      return "EXITED";
    case FAILURE:     return "FAILURE";
    }
}

static size_t StateToJson(const wapp_state_t *stateList, size_t stateLen, uint8_t *buf, size_t bufLen)
{
    char *p = (char *)buf;
    size_t left = bufLen;
    char ver[9];

    p = json_objOpen(p, NULL, &left);
    p = json_arrOpen(p, "wapps", &left);
    for (int i = 0; i < stateLen; i++) {
        p = json_objOpen(p, NULL, &left);
        p = json_str(p, "name", stateList[i].name, &left);
        p = json_int(p, "id", stateList[i].id, &left);
        p = json_str(p, "state", statusToString(stateList[i].status), &left);
        const uint8_t *v = stateList[i].version.v;
        snprintf(ver, 9, "%X.%X.%X", v[0], v[1], v[2]);
        p = json_nstr(p, "version", ver, 9, &left);
        p = json_objClose(p, &left);
    }
    p = json_arrClose(p, &left);
    p = json_objClose(p, &left);
    p = json_end(p, &left);

    return bufLen - left;
}

int WantedSetConfig(wantedConfig_t cfg)
{
    currentConfig = cfg;

    return 0;
}

int WantedGetConfig(uint8_t *buf, size_t bufLen)
{
    if (buf == NULL) return -EINVAL;
    return ConfigToJson(&currentConfig, buf, bufLen);
}

int WantedReadRegistry(uint8_t *buf, size_t bufLen)
{
    reg_entry_t entries[MAX_WAPPS];
    int n, ret;

    if (buf == NULL) return -EINVAL;

    n = PlatformRegistryRead(entries, MAX_WAPPS);
    if (n < 0) return n;

    return RegistryToJson((const reg_entry_t *)&entries, n, buf, bufLen);
}

int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen)
{
    if (buf == NULL) return -EINVAL;

    if (*cont == false) {
        *cont = true;
        return PlatformRegistryWrite(START_WRITE, buf, bufLen);
    }

    return PlatformRegistryWrite(CONTINUE_WRITE, buf, bufLen);
}

int WantedRegistryRemove(const reg_entry_t *entry)
{
    if (entry == NULL) return -EINVAL;

    return PlatformRegistryRemove(entry);
}

int WantedCloseRegistry()
{
    return PlatformRegistryWrite(FINISH_WRITE, NULL, 0);
}

int WantedReadState(uint8_t *buf, size_t bufLen)
{
    wapp_state_t wapps[MAX_WAPPS];

    int ret = PlatformWappGetState(wapps, MAX_WAPPS);
    if (ret < 0) return ret;

    return StateToJson(wapps, ret, buf, bufLen);
}

int WantedReadManifest(reg_entry_t *entry, uint8_t *buf, size_t bufLen)
{
    int ret;
    wapp_t w;
    uint8_t *m;
    size_t mLen;

    if (buf == NULL) return -EINVAL;

    ret = PlatformRegistryWappLoad(entry, &w);
    if (ret < 0) {
        return ret;
    }

    ret = WantedWappLoadManifest(&w, &m, &mLen);
    if (ret < 0) {
        return -EINVAL;
    }

    size_t n = mLen < bufLen ? mLen : bufLen;

    memcpy(buf, m, n);

    PlatformWappUnload(&w);

    return (int)n;
}

int WantedInstallDriver(struct vfs_ctx_t *c, const wapp_t *w, const char *name, const char *path, const char *options)
{
    int ret = 0;
    vfs_driver_t *drv = NULL;

    if (c == NULL || w == NULL || name == NULL || path == NULL) {
        return -EINVAL;
    }

    // TODO: make this simpler, e.g. a table of drivers
    if (memcmp("9p", name, 3) == 0) {
        drv = Vfs9PInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("can't load 9p driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("null", name, 5) == 0) {
        drv = VfsNullInit(w, 0, NULL);
        if (NULL == drv) {
            DEBUG_TRACE("can't load null driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("virt", name, 5) == 0) {
        drv = VfsVirtualInit(w, 0, NULL);
        if (NULL == drv) {
            DEBUG_TRACE("can't load virt driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("rom", name, 4) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }
        drv = VfsRomfsInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("can't load romfs (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("platform", name, 9) == 0) {
        uint8_t argc = 1;
        if (options == NULL) {
            argc = 0;
        }

        drv = VfsPlatformFsInit(w, argc, &options);
        if (NULL == drv) {
            DEBUG_TRACE("can't load platform driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("socket", name, 7) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }

        drv = VfsSocketInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("can't load socket driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("wanted", name, 7) == 0) {
        drv = VfsWantedInit(w, 0, NULL);
        if (NULL == drv) {
            DEBUG_TRACE("can't load wanted driver (%d)", ret);
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    ret = VfsRegister(c, path, drv);

    return ret;
}

int WantedParseCtrlAction(const char *buf, size_t bufLen, char *wappName, wapp_action_t *act, wapp_config_t *cfg)
{
    if (NULL == buf || NULL == wappName || NULL == cfg || NULL == act) return -EINVAL;

    int i = 0;
    json_t m[40];
    char b[bufLen];

    memcpy(b, buf, bufLen);
    memset(cfg, 0, sizeof(wapp_config_t));

    json_t const* json = json_create(b, m, sizeof m / sizeof *m);
    if (!json || JSON_OBJ != json_getType(json)) {
        DEBUG_TRACE("can't initialize json parser");
        return -ENOMEM;
    }

    json_t const* action = json_getProperty(json, "action");
    if (!action || JSON_TEXT != json_getType(action)) {
        DEBUG_TRACE(".action property not found in json");
        return -EINVAL;
    }

    json_t const* params = json_getProperty(json, "params");
    if (!params || JSON_OBJ != json_getType(params)) {
        DEBUG_TRACE(".params property not found in json");
        return -EINVAL;
    }

    json_t const* name = json_getProperty(params, "name");
    if (!name || JSON_TEXT != json_getType(name)) {
        DEBUG_TRACE(".params.name property not found in json");
        return -EINVAL;
    }
    strcpy(wappName, json_getValue(name));

    if (strcmp("start", json_getValue(action)) == 0) {
        *act = WAPP_START;

        json_t const* console = json_getProperty(params, "console");
        if (console && JSON_OBJ == json_getType(console)) {
            json_t const* in = json_getProperty(console, "in");
            if (in && JSON_OBJ == json_getType(in)) {
                strcpy(cfg->console[0].name,    NULL == json_getPropertyValue(in, "name") ? "" : json_getPropertyValue(in, "name"));
                strcpy(cfg->console[0].options, NULL == json_getPropertyValue(in, "options") ? "" : json_getPropertyValue(in, "options"));
            }

            json_t const* out = json_getProperty(console, "out");
            if (out && JSON_OBJ == json_getType(out)) {
                strcpy(cfg->console[1].name,    NULL == json_getPropertyValue(out, "name") ? "" : json_getPropertyValue(out, "name"));
                strcpy(cfg->console[1].options, NULL == json_getPropertyValue(out, "options") ? "" : json_getPropertyValue(out, "options"));
            }

            json_t const* err = json_getProperty(console, "err");
            if (err && JSON_OBJ == json_getType(err)) {
                strcpy(cfg->console[2].name,    NULL == json_getPropertyValue(err, "name") ? "" : json_getPropertyValue(err, "name"));
                strcpy(cfg->console[2].options, NULL == json_getPropertyValue(err, "options") ? "" : json_getPropertyValue(err, "options"));
            }
        }

        json_t const* drivers = json_getProperty(params, "drivers");
        json_t const* drv;
        if (drivers && JSON_ARRAY == json_getType(drivers)) {
            for(i = 0, drv = json_getChild(drivers); drv && i < 10; drv = json_getSibling(drv), i++) {
                if (JSON_OBJ == json_getType(drv)) {
                    strcpy(cfg->drivers[i].name, NULL == json_getPropertyValue(drv, "name") ? "" : json_getPropertyValue(drv, "name"));
                    strcpy(cfg->drivers[i].path, NULL == json_getPropertyValue(drv, "path") ? "" : json_getPropertyValue(drv, "path"));
                    strcpy(cfg->drivers[i].options, NULL == json_getPropertyValue(drv, "options") ? "" : json_getPropertyValue(drv, "options"));
                }
            }
            cfg->driversCnt = i;
        }
    } else if (strcmp("stop",  json_getValue(action)) == 0) {
        *act = WAPP_STOP;
    }

    return 0;
}
