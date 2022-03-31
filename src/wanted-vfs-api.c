#include <stdio.h>
#include <string.h>

#include <wanted-vfs-api.h>
#include <platform.h>

#include <wanted_malloc.h>
#include <mtojson.h>

static wantedConfig_t currentConfig;

#define SUBTRACT_OR_ZERO(a,b) ((a) > (b) ? (a) - (b) : 0)


static size_t ConfigToJson(const wantedConfig_t *cfg, uint8_t *buf, size_t bufLen)
{
    size_t len = cfg->nWapps;

    struct to_json *wappsArray = WantedMalloc((len+1) * sizeof(*wappsArray));
    memset(wappsArray, 0, sizeof(*wappsArray) * (len+1));

    for (int i = 0; i < len; i++) {
        wappsArray[i].value = cfg->wappsToRun[i];
        wappsArray[i].vtype = t_to_string;
    }

    const struct to_json json[] = {
        {
            .name = "wapps",
            .value = wappsArray,
            .vtype = t_to_array,
            .stype = t_to_object
        },
        { NULL }
    };

    len = json_generate(buf, json, bufLen);

    WantedFree(wappsArray);

    return len;
}

static size_t RegistryToJson(const reg_entry_t *reg, size_t regLen, uint8_t *buf, size_t bufLen)
{
    size_t len = regLen;
    size_t subElemLen = 3;

    struct to_json *wappsArray = WantedMalloc((subElemLen * len) * sizeof(*wappsArray));
    memset(wappsArray, 0, sizeof(*wappsArray) * (subElemLen * len));

    wappsArray[0].stype = t_to_array;
    for (int i = 0; i < len; i++) {
        wappsArray[i*subElemLen].name = "name";
        wappsArray[i*subElemLen].value = reg[i].name;
        wappsArray[i*subElemLen].vtype = t_to_string;
        wappsArray[i*subElemLen+1].name = "size";
        wappsArray[i*subElemLen+1].value = &reg[i].size;
        wappsArray[i*subElemLen+1].vtype = t_to_int;
    }

    size_t a = 3;
    const struct to_json json[] = {
        {
            .name = "wapps",
            .value = wappsArray,
            .count = &a,
            .vtype = t_to_object,
            .stype = t_to_object
        },
        { NULL }
    };

    len = json_generate(buf, json, bufLen);

    WantedFree(wappsArray);

    return len;
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
    size_t len = stateLen;
    size_t subElemLen = 4;

    struct to_json *wappsArray = WantedMalloc((subElemLen * len) * sizeof(*wappsArray));
    memset(wappsArray, 0, sizeof(*wappsArray) * (subElemLen * len));

    wappsArray[0].stype = t_to_array;
    for (int j = 0; j < subElemLen; j++)
        for (int i = 0; i < len; i++) {
            wappsArray[i*3].name    = "name";
            wappsArray[i*3].value   = stateList[i].name;
            wappsArray[i*3].vtype   = t_to_string;
            wappsArray[i*3+1].name  = "status";
            wappsArray[i*3+1].value = statusToString(stateList[i].status);
            wappsArray[i*3+1].vtype = t_to_string;
            wappsArray[i*3+2].name  = "version";
            wappsArray[i*3+2].value = stateList[i].version.v;
            wappsArray[i*3+2].vtype = t_to_hex_u32;
        }

    const struct to_json json[] = {
        {
            .name = "wapps",
            .value = wappsArray,
            .count = &len,
            .vtype = t_to_object,
            .stype = t_to_object
        },
        { NULL }
    };

    len = json_generate(buf, json, bufLen);

    WantedFree(wappsArray);

    return len;
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
