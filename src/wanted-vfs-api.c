#include <stdio.h>
#include <string.h>

#include <wanted-vfs-api.h>
#include <platform.h>

static wantedConfig_t currentConfig;

#define SUBTRACT_OR_ZERO(a,b) ((a) > (b) ? (a) - (b) : 0)


static size_t ConfigToJson(const wantedConfig_t *cfg, uint8_t *buf, size_t bufLen)
{
    int pos;
    size_t left = bufLen;

    pos = snprintf(buf, bufLen,
        "{"
        "\"wapps\":["
    );

    buf += pos;
    left = SUBTRACT_OR_ZERO(left, pos);

    for (int i = 0; i < cfg->nWapps && left > 0; i++) {
        pos = snprintf(buf, left, "\"%s\"", cfg->wappsToRun[i]);

        if (pos >= left) break;

        buf += pos;
        left = SUBTRACT_OR_ZERO(left, pos);

        if (i != cfg->nWapps-1) {
            pos = snprintf(buf, left, ",");
            buf += pos;
            left = SUBTRACT_OR_ZERO(left, pos);
        }
    }

    if (left == 0) return bufLen;

    pos = snprintf(buf, left, "]}\n");

    left = SUBTRACT_OR_ZERO(left, pos);

    return bufLen - left;
}

static size_t RegistryToJson(const reg_entry_t *reg, size_t regLen, uint8_t *buf, size_t bufLen)
{
    int pos;
    size_t left = bufLen;

    pos = snprintf(buf, bufLen,
        "{"
        "\"wapps\":["
    );

    buf += pos;
    left = SUBTRACT_OR_ZERO(left, pos);

    for (int i = 0; i < regLen && left > 0; i++) {
        pos = snprintf(buf, left, "{\"name\":\"%s\",\"size\":%d}", reg[i].name, reg[i].size);

        if (pos >= left) break;

        buf += pos;
        left = SUBTRACT_OR_ZERO(left, pos);

        if (i != regLen - 1) {
            pos = snprintf(buf, left, ",");
            buf += pos;
            left = SUBTRACT_OR_ZERO(left, pos);
        }
    }

    if (left == 0) return bufLen;

    pos = snprintf(buf, left, "]}\n");

    left = SUBTRACT_OR_ZERO(left, pos);

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
    int pos;
    size_t left = bufLen;

    pos = snprintf(buf, bufLen,
        "{"
        "\"wapps\":["
    );

    buf += pos;
    left = SUBTRACT_OR_ZERO(left, pos);

    for (int i = 0; i < stateLen && left > 0; i++) {
        pos = snprintf(buf, left, "{\"name\":\"%s\",\"status\":\"%s\",\"version\":\"%d.%d.%d\"}",
            stateList[i].name,
            statusToString(stateList[i].status),
            stateList[i].version.major, stateList[i].version.minor, stateList[i].version.patch);

        if (pos >= left) break;

        buf += pos;
        left = SUBTRACT_OR_ZERO(left, pos);

        if (i != stateLen - 1) {
            pos = snprintf(buf, left, ",");
            buf += pos;
            left = SUBTRACT_OR_ZERO(left, pos);
        }
    }

    if (left == 0) return bufLen;

    pos = snprintf(buf, left, "]}\n");

    left = SUBTRACT_OR_ZERO(left, pos);

    return bufLen - left;
}

int WantedSetConfig(wantedConfig_t cfg)
{
    currentConfig = cfg;
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

    return RegistryToJson((const reg_entry_t *)&entries, n, buf, bufLen);
}

int WantedReadState(uint8_t *buf, size_t bufLen)
{
    wapp_state_t wapps[MAX_WAPPS];

    int ret = GetState(wapps, MAX_WAPPS);

    return StateToJson(wapps, ret, buf, bufLen);
}
