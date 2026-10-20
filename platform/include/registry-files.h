/* SPDX-License-Identifier: Apache-2.0 */

/* Small named records, one file each, in one directory of a POSIX filesystem.
 * A put lands the record whole or not at all: it is staged in the directory
 * and renamed over the target. Names that start with '.' or '_' are skipped. */

#ifndef REGISTRY_FILES_H
#define REGISTRY_FILES_H

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <platform-config.h>
#include <registry-store.h>

#define REGISTRY_REC_EXT ".rec"

int RegFilesPut(const char *dir, const char *name, const uint8_t *data,
                size_t len);
/* Bytes read, -ENOENT, or -EMSGSIZE when the record is longer than `cap`. */
int RegFilesGet(const char *dir, const char *name, uint8_t *buf, size_t cap);
int RegFilesDel(const char *dir, const char *name);
/* Calls `visit` for each name until it returns non-zero. Returns the number of
 * names visited, or a negative errno. A missing directory has none. */
int RegFilesList(const char *dir, int (*visit)(const char *name, void *arg),
                 void *arg);

/* The file name of an entry record, "<name><sep><version>.rec", and back. */
static inline int RegFilesKeyName(char *out, size_t cap,
                                  const registry_key_t *key) {
    int n =
        snprintf(out, cap, "%s%c%s%s", key->name, REGISTRY_VERSION_SEPARATOR,
                 key->version, REGISTRY_REC_EXT);

    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static inline bool RegFilesParseKey(const char *file, registry_key_t *key) {
    size_t len = strlen(file);
    size_t extLen = sizeof(REGISTRY_REC_EXT) - 1;
    const char *sep = strchr(file, REGISTRY_VERSION_SEPARATOR);
    size_t nameLen;
    size_t verLen;

    if (len <= extLen || strcmp(file + len - extLen, REGISTRY_REC_EXT) != 0 ||
        sep == NULL)
        return false;
    nameLen = (size_t)(sep - file);
    verLen = len - extLen - nameLen - 1;
    if (nameLen == 0 || nameLen >= REGISTRY_KEY_NAME_LEN ||
        verLen >= REGISTRY_KEY_NAME_LEN)
        return false;
    memset(key, 0, sizeof(*key));
    memcpy(key->name, file, nameLen);
    memcpy(key->version, sep + 1, verLen);
    return true;
}

#endif /* REGISTRY_FILES_H */
