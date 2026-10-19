/* SPDX-License-Identifier: Apache-2.0 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include <platform-config.h>
#include <registry-files.h>

#define STAGE_NAME "_stage"

static int pathOf(char *out, size_t cap, const char *dir, const char *name) {
    int n = snprintf(out, cap, "%s/%s", dir, name);

    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

int RegFilesPut(const char *dir, const char *name, const uint8_t *data,
                size_t len) {
    char stage[PATH_MAX];
    char path[PATH_MAX];
    FILE *f;
    size_t w;
    int closed;
    int rc = pathOf(stage, sizeof(stage), dir, STAGE_NAME);

    if (rc == 0)
        rc = pathOf(path, sizeof(path), dir, name);
    if (rc < 0)
        return rc;
    f = fopen(stage, "wb");
    if (f == NULL)
        return -errno;
    w = fwrite(data, 1, len, f);
    closed = fclose(f);
    if (w != len || closed != 0) {
        remove(stage);
        return -EIO;
    }
    if (rename(stage, path) < 0) {
        rc = -errno;
        remove(stage);
        return rc;
    }
    return 0;
}

int RegFilesGet(const char *dir, const char *name, uint8_t *buf, size_t cap) {
    char path[PATH_MAX];
    FILE *f;
    size_t r;
    int rc = pathOf(path, sizeof(path), dir, name);

    if (rc < 0)
        return rc;
    f = fopen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    r = fread(buf, 1, cap, f);
    rc = fgetc(f) != EOF ? -EMSGSIZE : (int)r;
    fclose(f);
    return rc;
}

int RegFilesDel(const char *dir, const char *name) {
    char path[PATH_MAX];
    int rc = pathOf(path, sizeof(path), dir, name);

    if (rc < 0)
        return rc;
    return remove(path) == 0 ? 0 : -errno;
}

int RegFilesList(const char *dir, int (*visit)(const char *name, void *arg),
                 void *arg) {
    DIR *d = opendir(dir);
    const struct dirent *de;
    int n = 0;

    if (d == NULL)
        return errno == ENOENT ? 0 : -errno;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' || de->d_name[0] == '_')
            continue;
        n++;
        if (visit(de->d_name, arg) != 0)
            break;
    }
    closedir(d);
    return n;
}

#define REC_EXT ".rec"

int RegFilesKeyName(char *out, size_t cap, const registry_key_t *key) {
    int n = snprintf(out, cap, "%s%c%s%s", key->name,
                     REGISTRY_VERSION_SEPARATOR, key->version, REC_EXT);

    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

bool RegFilesParseKey(const char *file, registry_key_t *key) {
    size_t len = strlen(file);
    size_t extLen = sizeof(REC_EXT) - 1;
    const char *sep;
    size_t nameLen;
    size_t verLen;

    if (len <= extLen || strcmp(file + len - extLen, REC_EXT) != 0)
        return false;
    sep = strchr(file, REGISTRY_VERSION_SEPARATOR);
    if (sep == NULL)
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
