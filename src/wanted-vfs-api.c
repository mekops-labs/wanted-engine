#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include <debug_trace.h>
#include <wanted-vfs-api.h>
#include <platform.h>
#include <vfs-drivers.h>

#include <json-maker/json-maker.h>

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

    if (memcmp("virt", name, 5) == 0) {
        drv = VfsVirtualInit(w, 0, NULL);
        if (NULL == drv) {
            DEBUG_TRACE("VfsVirtualInit: can't load virt driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("rom", name, 4) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }
        drv = VfsRomfsInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("VfsRomfsInit: can't load romfs (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("platform", name, 9) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }

        drv = VfsPlatformFsInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("VfsPlatformInit: can't load platform driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("socket", name, 7) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }

        drv = VfsSocketInit(w, 1, &options);
        if (NULL == drv) {
            DEBUG_TRACE("VfsPlatformInit: can't load platform driver (%d)", ret);
            return -EINVAL;
        }
    } else if (memcmp("wanted", name, 7) == 0) {
        drv = VfsWantedInit(w, 0, NULL);
        if (NULL == drv) {
            DEBUG_TRACE("VfsWantedInit: can't load wanted driver (%d)", ret);
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    ret = VfsRegister(c, path, drv);

    return ret;
}
