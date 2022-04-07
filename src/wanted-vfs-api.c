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

#define SUBTRACT_OR_ZERO(a,b) ((a) > (b) ? (a) - (b) : 0)


static size_t ConfigToJson(const wantedConfig_t *cfg, uint8_t *buf, size_t bufLen)
{
    char *p = buf;
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
    char *p = buf;
    size_t left = bufLen;

    p = json_objOpen(p, NULL, &left);
    p = json_arrOpen(p, "wapps", &left);
    for (int i = 0; i < regLen; i++) {
        p = json_objOpen(p, NULL, &left);
        p = json_str(p, "name", reg[i].name, &left);
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
    char *p = buf;
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
    if (buf == NULL) return -1;
    return ConfigToJson(&currentConfig, buf, bufLen);
}

int WantedReadRegistry(uint8_t *buf, size_t bufLen)
{
    reg_entry_t entries[MAX_WAPPS];
    int n;

    if (buf == NULL) return -1;

    n = PlatformRegistryRead(entries, MAX_WAPPS);
    if (n < 0) return n;

    return RegistryToJson((const reg_entry_t *)&entries, n, buf, bufLen);
}

int WantedWriteRegistry(bool *cont, const uint8_t *buf, size_t bufLen)
{
    if (buf == NULL) return -1;

    if (*cont == false) {
        *cont = true;
        return PlatformRegistryWrite(START_WRITE, buf, bufLen);
    }

    return PlatformRegistryWrite(CONTINUE_WRITE, buf, bufLen);
}

int WantedRegistryRemove(const char *name)
{
    if (name == NULL) return -1;

    return PlatformRegistryRemove(name);
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

int WantedInstallDriver(struct vfs_ctx_t *c, const wapp_t *w, const char *name, const char *path, const char *options)
{
    int ret = 0;
    vfs_driver_t drv = { 0 };

    if (c == NULL || w == NULL || name == NULL || path == NULL) {
        return -EINVAL;
    }

    if (memcmp("virt", name, 5) == 0) {
        ret = VfsVirtualInit(&drv);
        if (ret < 0) {
            DEBUG_TRACE("VfsVirtualInit: can't load virt driver (%d)", ret);
            return ret;
        }
    } else if (memcmp("rom", name, 4) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }
        ret = VfsRomfsInit(&drv, options, w->img, w->img_len);
        if (ret < 0) {
            DEBUG_TRACE("VfsRomfsInit: can't load romfs (%d)", ret);
            return ret;
        }
    } else if (memcmp("platform", name, 9) == 0) {
        ret = VfsPlatformFsInit(&drv);
        if (ret < 0) {
            DEBUG_TRACE("VfsPlatformInit: can't load platform driver (%d)", ret);
            return ret;
        }
    } else if (memcmp("socket", name, 7) == 0) {
        if (options == NULL) {
            return -EINVAL;
        }
        char t;
        char host[strlen(options)];
        uint16_t port;
        ret = sscanf(options, "%c %s %d", &t, host, &port);
        if (ret < 3) {
            DEBUG_TRACE("VfsSocketInit: bad options");
            return -EINVAL;
        }

        uint8_t type;
        switch (t) {
            case 't': type = VFS_SKT_TCP; break;
            case 'u': type = VFS_SKT_UDP; break;
            case 'b': type = VFS_SKT_BUS; break;
            default: return -EINVAL;
        }

        ret = VfsSocketInit(&drv, type, host, port);
        if (ret < 0) {
            DEBUG_TRACE("VfsPlatformInit: can't load platform driver (%d)", ret);
            return ret;
        }
    } else if (memcmp("wanted", name, 7) == 0) {
        ret = VfsVirtualInit(&drv);
        if (ret < 0) {
            DEBUG_TRACE("VfsWantedInit: can't load driver (%d)", ret);
            return ret;
        }
        ret = drv.Register(drv.ctx, "config", &WantedConfigDriver);
        if (ret < 0) {
            DEBUG_TRACE("VfsWantedInit: can't register config (%d)", ret);
            return ret;
        }
        ret = drv.Register(drv.ctx, "ctrl", &WantedControlDriver);
        if (ret < 0) {
            DEBUG_TRACE("VfsWantedInit: can't register ctrl (%d)", ret);
            return ret;
        }
        ret = drv.Register(drv.ctx, "reg", &WantedRegistryDriver);
        if (ret < 0) {
            DEBUG_TRACE("VfsWantedInit: can't register reg (%d)", ret);
            return ret;
        }
    } else {
        return -EINVAL;
    }

    ret = VfsRegister(c, path, &drv);

    return ret;
}
