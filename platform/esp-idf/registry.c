/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF wapp registry index: enumerates the LittleFS "registry" directory. */

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <config-esp-idf.h>
#include <platform.h>
#include <registry-image.h>
#include <wanted.h>

static inline size_t min(size_t a, size_t b) { return (a) > (b) ? (b) : (a); }

static bool hasRegistryExt(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) {
        return false;
    }
    return strcmp(ext, REGISTRY_EXT) == 0;
}

static int nameLenWithoutExt(const char *name) {
    const char *ext = strrchr(name, '.');
    if ((!ext) || (ext == name)) {
        return 0;
    }
    return (int)(ext - name);
}

/* Split a registry filename "<name>@<version>.wapp" into name/version. */
static void parseEntry(reg_entry_t *out, const char *dname) {
    size_t entryNameLen = min(nameLenWithoutExt(dname) + 1,
                              WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN);
    const char *ver = strchr(dname, (int)REGISTRY_VERSION_SEPARATOR);
    size_t nameLen = entryNameLen;

    out->version[0] = '\0';
    if (ver != NULL) {
        ver += 1;
        nameLen = ver - dname;
        size_t verLen = entryNameLen - nameLen;

        strncpy(out->version, ver, verLen);
        out->version[verLen - 1] = '\0';
    }

    strncpy(out->name, dname, nameLen);
    out->name[nameLen - 1] = '\0';
}

static int compareEntries(const void *a, const void *b) {
    const reg_entry_t *ea = (const reg_entry_t *)a;
    const reg_entry_t *eb = (const reg_entry_t *)b;
    int c = strcmp(ea->name, eb->name);
    if (c != 0) {
        return c;
    }
    return strcmp(ea->version, eb->version);
}

/* Read a metadata file's recorded image size; 0 if the file is unreadable or
 * not a valid record (e.g. truncated by a crash mid-install). */
/* Stored image length, or -1 where no read could resolve this entry. An entry
 * left by another slot stride is not listed: advertising one whose bytes have
 * moved leaves a supervisor relaunching an image it can never load. */
static long readMetaSize(const char *path) {
    wapp_image_meta_t meta;
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    size_t r = fread(&meta, 1, sizeof(meta), f);
    fclose(f);
    if (!WappImageMetaValid(&meta, r, WAPP_IMAGE_SLOT_SIZE))
        return -1;
    return (long)meta.size;
}

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) {
    DIR *dir;
    const struct dirent *de;
    char path[WAPP_REG_PATH_MAX];
    struct stat s;
    size_t filled = 0;
    int count = 0;

    dir = opendir(REGISTRY_ROOT);
    if (dir == NULL) {
        if (ENOENT == errno) {
            if (mkdir(REGISTRY_ROOT, 0755) < 0) {
                return -errno;
            }
            return 0;
        }
        return -errno;
    }

    while ((de = readdir(dir)) != NULL) {
        if (!hasRegistryExt(de->d_name)) {
            continue;
        }

        /* de->d_name's declared width exceeds WAPP_REG_PATH_MAX, but a name
         * reaching here passed the ".wapp" check and is always short. Skip a
         * would-be-truncated entry rather than sizing for the worst case. */
        int n =
            snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, de->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) {
            continue;
        }
        if (stat(path, &s) < 0 || !S_ISREG(s.st_mode)) {
            continue;
        }

        long size = readMetaSize(path);
        if (size < 0) {
            continue;
        }

        if (registryList != NULL && filled < len) {
            parseEntry(&registryList[filled], de->d_name);
            registryList[filled].size = (size_t)size;
            filled++;
        }
        count++;
    }

    closedir(dir);

    if (registryList != NULL) {
        qsort(registryList, filled, sizeof(reg_entry_t), compareEntries);
    }

    return count;
}

/* Images live in fixed raw-flash slots; the count is the hard ceiling a
 * supervisor needs to see before it runs out. */
size_t PlatformRegistrySlots(void) { return WAPP_IMAGE_MAX_SLOTS; }
