/* SPDX-License-Identifier: Apache-2.0 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <config-nuttx.h>
#include <debug_trace.h>
#include <registry.h>

#include "wanted_seed.h"

#define SEED_COPY_BUF 1024

static reg_entry_t installed[REGISTRY_MAX_ENTRIES];

/* "<name>@<version>.wapp" into the ref "<name>:<version>". */
static bool refOf(const char *file, char *ref, size_t cap) {
    size_t ext = sizeof(REGISTRY_EXT) - 1;
    size_t len = strlen(file);
    char *sep;

    if (len <= ext || len - ext >= cap ||
        strcmp(file + len - ext, REGISTRY_EXT))
        return false;
    memcpy(ref, file, len - ext);
    ref[len - ext] = '\0';
    sep = strchr(ref, REGISTRY_VERSION_SEPARATOR);
    if (sep != NULL)
        *sep = ':';
    return true;
}

static bool isInstalled(const char *ref) {
    int n = RegistryRead(installed, REGISTRY_MAX_ENTRIES);
    const char *colon = strchr(ref, ':');
    size_t nameLen = colon != NULL ? (size_t)(colon - ref) : strlen(ref);
    const char *version = colon != NULL ? colon + 1 : "";

    for (int i = 0; i < n && i < REGISTRY_MAX_ENTRIES; i++) {
        if (strlen(installed[i].name) == nameLen &&
            strncmp(installed[i].name, ref, nameLen) == 0 &&
            strcmp(installed[i].version, version) == 0)
            return true;
    }
    return false;
}

static void seedOne(const char *path, const char *ref) {
    char buf[SEED_COPY_BUF];
    int in = open(path, O_RDONLY);
    bool started = false;
    ssize_t n;

    if (in < 0)
        return;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        int rc = RegistryWrite(started ? CONTINUE_WRITE : START_WRITE, ref,
                               (const uint8_t *)buf, (size_t)n);

        if (rc < 0) {
            if (started)
                RegistryWrite(ABORT_WRITE, NULL, NULL, 0);
            close(in);
            return;
        }
        started = true;
    }
    close(in);
    if (started)
        RegistryWrite(FINISH_WRITE, NULL, NULL, 0);
}

void WantedSeedRegistry(const char *dir) {
    DIR *d = opendir(dir);
    const struct dirent *e;

    if (d == NULL) {
        DEBUG_TRACE("opendir(%s) failed: %s", dir, strerror(errno));
        return;
    }
    while ((e = readdir(d)) != NULL) {
        char ref[64];
        char path[256];

        if (e->d_name[0] == '.' || !refOf(e->d_name, ref, sizeof(ref)))
            continue;
        if (isInstalled(ref))
            continue;
        DEBUG_TRACE("seeding %s", ref);
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        seedOne(path, ref);
    }
    closedir(d);
}
