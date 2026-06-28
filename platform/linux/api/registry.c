/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <config-linux.h>
#include <platform.h>
#include <wanted.h>

/* Upper bound on entries scanned when resolving a bare name; matches the
 * registry capacity the VFS layer exposes. Avoids a variable-length array. */
#define REGISTRY_MAX_ENTRIES 50

static inline size_t min(size_t a, size_t b) { return (a) > (b) ? (b) : (a); }

/* when return 1, scandir will put this dirent to the list */
static int parseExt(const struct dirent *dir) {
    if (!dir) {
        return 0;
    }

    if (dir->d_type == DT_REG) { /* only deal with regular file */
        const char *ext = strrchr(dir->d_name, '.');
        if ((!ext) || (ext == dir->d_name)) {
            return 0;
        }
        if (strcmp(ext, REGISTRY_EXT) == 0) {
            return 1;
        }
    }

    return 0;
}

static int nameLenWithoutExt(const char *name) {
    const char *ext = strrchr(name, '.');
    if ((!ext) || (ext == name)) {
        return 0;
    }
    return (int)(ext - name);
}

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) {
    struct dirent **namelist;
    struct stat s;
    int n, i = 0;
    int d;

    d = open(REGISTRY_ROOT, O_DIRECTORY | O_RDONLY);
    if (d < 0) {
        if (ENOENT == errno) {
            if (mkdir(REGISTRY_ROOT, 0755) < 0) {
                return -errno;
            }

            return 0;
        }
        return -errno;
    }

    n = scandir(REGISTRY_ROOT, &namelist, parseExt, alphasort);
    if (n < 0) {
        close(d);
        return -errno;
    }

    for (i = 0; i < n; i++, len--) {
        if (registryList != NULL && len > 0) {
            size_t entryNameLen =
                min(nameLenWithoutExt(namelist[i]->d_name) + 1,
                    WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN);
            const char *ver =
                strchr(namelist[i]->d_name, (int)REGISTRY_VERSION_SEPARATOR);
            size_t nameLen = entryNameLen;

            if (ver != NULL) {
                ver += 1;
                nameLen = ver - namelist[i]->d_name;
                size_t verLen = entryNameLen - nameLen;

                strncpy(registryList[i].version, ver, verLen);
                registryList[i].version[verLen - 1] = '\0';
            }

            strncpy(registryList[i].name, namelist[i]->d_name, nameLen);
            registryList[i].name[nameLen - 1] = '\0';

            if (fstatat(d, namelist[i]->d_name, &s, 0) == 0) {
                registryList[i].size = s.st_size;
            } else {
                registryList[i].size = 0;
            }
        }

        free(namelist[i]);
    }

    free((void *)namelist);

    close(d);

    return i;
}

int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    char targetName[PATH_MAX];
    reg_entry_t resolved;
    int ret;

    if (!entry->version[0]) {
        /* Resolve a bare name to the first registry entry that carries it. */
        int num = PlatformRegistryRead(NULL, 0);
        if (num < 0)
            return num;
        if (num > REGISTRY_MAX_ENTRIES)
            num = REGISTRY_MAX_ENTRIES;

        reg_entry_t list[REGISTRY_MAX_ENTRIES];
        num = PlatformRegistryRead(list, num);

        const reg_entry_t *match = NULL;
        for (int i = 0; i < num; i++) {
            if (strncmp(list[i].name, entry->name, WAPP_MAX_NAME_LEN) == 0) {
                match = &list[i];
                break;
            }
        }
        if (match == NULL)
            return -ENOENT;
        resolved = *match;
    } else {
        resolved = *entry;
    }

    snprintf(targetName, sizeof(targetName), "%s/%s%c%s%s", REGISTRY_ROOT,
             resolved.name, REGISTRY_VERSION_SEPARATOR, resolved.version,
             REGISTRY_EXT);

    ret = PlatformWappLoad(targetName, w);
    if (ret < 0)
        return ret;

    /* Image identity is the registry entry — name and version tag. The instance
     * name (w->name) is set by the launch path and left untouched here. */
    strncpy(w->image, resolved.name, WAPP_MAX_NAME_LEN - 1);
    w->image[WAPP_MAX_NAME_LEN - 1] = '\0';
    strncpy(w->version, resolved.version, WAPP_MAX_VERSION_LEN - 1);
    w->version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    return 0;
}

int PlatformRegistryReadImage(const reg_entry_t *entry, uint8_t *buf,
                              size_t maxLen) {
    if (entry == NULL || buf == NULL || maxLen == 0)
        return -EINVAL;

    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/%s%c%s%s", REGISTRY_ROOT,
                     entry->name, REGISTRY_VERSION_SEPARATOR, entry->version,
                     REGISTRY_EXT);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -ENAMETOOLONG;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    ssize_t r = read(fd, buf, maxLen);
    close(fd);
    return r < 0 ? -errno : (int)r;
}
