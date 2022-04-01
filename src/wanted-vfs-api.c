#include <stdio.h>
#include <string.h>

#include <wanted-vfs-api.h>
#include <platform.h>

#include <wanted_malloc.h>
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

    n = PlatformReadRegistry(entries, MAX_WAPPS);
    if (n < 0) return n;

    return RegistryToJson((const reg_entry_t *)&entries, n, buf, bufLen);
}

int WantedReadState(uint8_t *buf, size_t bufLen)
{
    wapp_state_t wapps[MAX_WAPPS];

    int ret = GetState(wapps, MAX_WAPPS);
    if (ret < 0) return ret;

    return StateToJson(wapps, ret, buf, bufLen);
}
