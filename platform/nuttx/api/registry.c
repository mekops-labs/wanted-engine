/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX platform wapp registry.
 *
 * The sim backs the registry on a hostfs directory. Reads enumerate it with
 * opendir/readdir (scandir is optional on NuttX, gated by CONFIG_LIBC_SCANDIR,
 * so it is avoided) and sort the result; the writer stages incoming bytes to a
 * temp file then renames it into place under the install ref ("<name>:<ver>")
 * once the stream completes. */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <config-nuttx.h>
#include <platform.h>
#include <wanted.h>
#include <wanted_malloc.h>

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

/* Split a registry filename "<name>:<version>.wapp" into the entry's name and
 * version fields, mirroring the Linux platform's parse. */
static void parseEntry(reg_entry_t *out, const char *dname, size_t size) {
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
    out->size = size;
}

/* Lexicographic comparison: safe (both fields are always null-terminated by
 * parseEntry), but version ordering is wrong for multi-digit fields —
 * "1.10.0-1" sorts before "1.9.0-1" because '1' < '9'. Acceptable while all
 * version fields remain single-digit; a numeric comparator is needed otherwise.
 */
static int compareEntries(const void *a, const void *b) {
    const reg_entry_t *ea = (const reg_entry_t *)a;
    const reg_entry_t *eb = (const reg_entry_t *)b;
    int c = strcmp(ea->name, eb->name);
    if (c != 0) {
        return c;
    }
    return strcmp(ea->version, eb->version);
}

int PlatformRegistryRead(reg_entry_t *registryList, size_t len) {
    DIR *dir;
    const struct dirent *de;
    char path[PATH_MAX];
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

        /* NuttX dirent d_type is unreliable across filesystems; stat instead.
         */
        snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, de->d_name);
        if (stat(path, &s) < 0 || !S_ISREG(s.st_mode)) {
            continue;
        }

        if (registryList != NULL && filled < len) {
            parseEntry(&registryList[filled], de->d_name, (size_t)s.st_size);
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

/* In-RAM image cache.
 *
 * On ESP32 an SPI-flash read returns corrupt data (LittleFS reports
 * LFS_ERR_CORRUPT) while another wapp holds live PSRAM — so the registry image
 * files cannot be read off flash once a wapp is running. The cache reads every
 * image into RAM the first time a wapp is started (when only the supervisor is
 * live, so the flash reads are safe) and serves every later launch from RAM, so
 * a second concurrent wapp never touches flash. Masters are kept for the device
 * lifetime; each launch gets its own copy (freed by PlatformWappUnload), so the
 * cache stays intact while instances come and go.
 *
 * Limit: an image installed *after* the first launch, then started while
 * another wapp runs, still falls back to a flash read (and can fail). Caching
 * on install is the matching follow-up. */
typedef struct {
    char ref[WAPP_MAX_IMAGE_REF_LEN]; /* "<name>:<version>" */
    uint8_t *data;
    size_t len;
} cache_entry_t;

static cache_entry_t imageCache[REGISTRY_MAX_ENTRIES];
static int imageCacheCount;
static bool imageCachePreloaded;

static void buildRef(char *out, size_t outLen, const char *name,
                     const char *version) {
    snprintf(out, outLen, "%s%c%s", name, REGISTRY_VERSION_SEPARATOR, version);
}

static cache_entry_t *cacheFind(const char *ref) {
    for (int i = 0; i < imageCacheCount; i++) {
        if (strcmp(imageCache[i].ref, ref) == 0)
            return &imageCache[i];
    }
    return NULL;
}

/* Read one image file into the cache, taking ownership of the loaded buffer.
 * Returns the new entry, or NULL if the cache is full or the read fails. */
static cache_entry_t *cacheAdd(const char *ref, const char *targetName) {
    wapp_t tmp;
    cache_entry_t *c;

    if (imageCacheCount >= REGISTRY_MAX_ENTRIES)
        return NULL;

    memset(&tmp, 0, sizeof(tmp));
    if (PlatformWappLoad(targetName, &tmp) < 0)
        return NULL;

    c = &imageCache[imageCacheCount];
    strncpy(c->ref, ref, sizeof(c->ref) - 1);
    c->ref[sizeof(c->ref) - 1] = '\0';
    c->data = tmp.layers[0];
    c->len = tmp.layer_lens[0];
    imageCacheCount++;
    return c;
}

/* Read every registry image into the cache. Called on the first launch, while
 * only the supervisor is running and flash reads are still safe. */
static void cachePreload(void) {
    reg_entry_t *list;
    char ref[WAPP_MAX_IMAGE_REF_LEN];
    char targetName[PATH_MAX];
    int num;

    imageCachePreloaded = true; /* one attempt; a re-entry must not loop */

    /* Heap (not stack): this can run on the init task's small stack, and the
     * LittleFS read path below is already stack-hungry. */
    list = (reg_entry_t *)WantedMalloc(sizeof(reg_entry_t) *
                                       REGISTRY_MAX_ENTRIES);
    if (list == NULL)
        return;

    num = PlatformRegistryRead(list, REGISTRY_MAX_ENTRIES);
    if (num < 0) {
        WantedFree(list);
        return;
    }
    if ((size_t)num > REGISTRY_MAX_ENTRIES)
        num = REGISTRY_MAX_ENTRIES;

    for (int i = 0; i < num; i++) {
        buildRef(ref, sizeof(ref), list[i].name, list[i].version);
        if (cacheFind(ref) != NULL)
            continue;
        snprintf(targetName, sizeof(targetName), "%s/%s%s", REGISTRY_ROOT, ref,
                 REGISTRY_EXT);
        cacheAdd(ref, targetName); /* best-effort; skip unreadable images */
    }

    WantedFree(list);
}

/* Preload the image cache from the boot shim, before the supervisor (and thus
 * WAMR/PSRAM) starts — the only moment ESP32 flash reads are reliably safe.
 * Idempotent; the lazy path in PlatformRegistryWappLoad covers platforms whose
 * shim does not call this (e.g. the sim). */
void RegistryCachePreload(void) {
    if (!imageCachePreloaded)
        cachePreload();
}

int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    char targetName[PATH_MAX];
    char ref[WAPP_MAX_IMAGE_REF_LEN];
    reg_entry_t resolved;
    cache_entry_t *c;
    uint8_t *copy;

    if (!entry->version[0]) {
        reg_entry_t list[REGISTRY_MAX_ENTRIES];
        const reg_entry_t *match = NULL;
        int num;

        num = PlatformRegistryRead(list, REGISTRY_MAX_ENTRIES);
        if (num < 0)
            return num;
        if ((size_t)num > REGISTRY_MAX_ENTRIES)
            num = REGISTRY_MAX_ENTRIES;

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

    buildRef(ref, sizeof(ref), resolved.name, resolved.version);
    snprintf(targetName, sizeof(targetName), "%s/%s%s", REGISTRY_ROOT, ref,
             REGISTRY_EXT);

    if (!imageCachePreloaded)
        cachePreload();

    c = cacheFind(ref);
    if (c == NULL)
        c = cacheAdd(ref, targetName); /* not preloaded (e.g. just installed) */
    if (c == NULL)
        return -ENOENT;

    /* Hand the launch its own copy of the cached image (RAM-to-RAM, no flash);
     * the master stays cached for the next launch. PSRAM-backed (freed by
     * PlatformWappUnload) so the per-launch image copy stays out of internal
     * RAM, which is reserved for task stacks. */
    copy = (uint8_t *)PlatformExtramMalloc(c->len);
    if (copy == NULL)
        return -ENOMEM;
    memcpy(copy, c->data, c->len);

    w->layers[0] = copy;
    w->layer_lens[0] = c->len;
    w->layer_cnt = 1;

    /* Image identity is the registry entry — name and version tag. The instance
     * name (w->name) is set by the launch path and left untouched here. */
    strncpy(w->image, resolved.name, WAPP_MAX_NAME_LEN - 1);
    w->image[WAPP_MAX_NAME_LEN - 1] = '\0';
    strncpy(w->version, resolved.version, WAPP_MAX_VERSION_LEN - 1);
    w->version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    return 0;
}
