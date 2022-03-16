#include <stdio.h>
#include <string.h>

#include <wanted-vfs-api.h>

static wantedConfig_t currentConfig;

#define SUBTRACT_OR_ZERO(a,b) ((a) > (b) ? (a) - (b) : 0)

static size_t ConfigToJson(uint8_t *buf, size_t bufLen)
{
    int pos;
    size_t left = bufLen;

    pos = snprintf(buf, bufLen,
        "{"
        "\"wapps\":["
    );

    buf += pos;
    left = SUBTRACT_OR_ZERO(left, pos);

    for (int i = 0; i < currentConfig.nWapps && left > 0; i++) {
        pos = snprintf(buf, left, "\"%s\"", currentConfig.wappsToRun[i]);

        if (pos >= left) break;

        buf += pos;
        left = SUBTRACT_OR_ZERO(left, pos);

        if (i != currentConfig.nWapps-1) {
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
    return ConfigToJson(buf, bufLen);
}
