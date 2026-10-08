/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <errno.h>
#include <stddef.h>

/* Splits the text after `connect` into an SSID and a passphrase, in place.
 * The SSID is a bare word or quoted text with `\"` and `\\` as escapes.
 * Returns -EINVAL for an empty SSID or a malformed quoted one. */
static inline int WifiCtlSplitConnect(char *args, char **ssid, char **pass) {
    char *p = args;
    while (*p == ' ')
        p++;
    if (*p == '\0')
        return -EINVAL;

    if (*p != '"') {
        *ssid = p;
        while (*p != '\0' && *p != ' ')
            p++;
        if (*p == ' ')
            *p++ = '\0';
        *pass = p;
        return 0;
    }

    char *out = ++p;
    *ssid = out;
    for (;; p++) {
        if (*p == '\0')
            return -EINVAL;
        if (*p == '"')
            break;
        if (*p == '\\') {
            p++;
            if (*p != '"' && *p != '\\')
                return -EINVAL;
        }
        *out++ = *p;
    }
    if (out == *ssid)
        return -EINVAL;
    p++;
    if (*p != '\0' && *p != ' ')
        return -EINVAL;
    *out = '\0';
    *pass = (*p == ' ') ? p + 1 : p;
    return 0;
}
