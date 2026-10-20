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
