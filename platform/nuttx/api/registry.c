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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <config-nuttx.h>
#include <platform.h>
#include <wanted.h>

static inline size_t min(size_t a, size_t b) { return (a) > (b) ? (b) : (a); }

static bool HasRegistryExt(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext || ext == name) {
        return false;
    }
    return strcmp(ext, REGISTRY_EXT) == 0;
}

static int NameLenWithoutExt(const char *name) {
    const char *ext = strrchr(name, '.');
    if ((!ext) || (ext == name)) {
        return 0;
    }
    return (int)(ext - name);
}

/* Split a registry filename "<name>:<version>.wapp" into the entry's name and
 * version fields, mirroring the Linux platform's parse. */
static void ParseEntry(reg_entry_t *out, const char *dname, size_t size) {
    size_t entryNameLen = min(NameLenWithoutExt(dname) + 1,
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
 * ParseEntry), but version ordering is wrong for multi-digit fields —
 * "1.10.0-1" sorts before "1.9.0-1" because '1' < '9'. Acceptable while all
 * version fields remain single-digit; a numeric comparator is needed otherwise.
 */
static int CompareEntries(const void *a, const void *b) {
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
    struct dirent *de;
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
        if (!HasRegistryExt(de->d_name)) {
            continue;
        }

        /* NuttX dirent d_type is unreliable across filesystems; stat instead.
         */
        snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, de->d_name);
        if (stat(path, &s) < 0 || !S_ISREG(s.st_mode)) {
            continue;
        }

        if (registryList != NULL && filled < len) {
            ParseEntry(&registryList[filled], de->d_name, (size_t)s.st_size);
            filled++;
        }
        count++;
    }

    closedir(dir);

    if (registryList != NULL) {
        qsort(registryList, filled, sizeof(reg_entry_t), CompareEntries);
    }

    return count;
}

int PlatformRegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                          size_t nbytes) {
    static FILE *f;
    static char tempName[] = REGISTRY_ROOT "/_temp";
    static char targetRef[PATH_MAX];
    static char targetName[PATH_MAX];

    int written = 0;

    switch (s) {
    case START_WRITE:
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        /* The install target is named by the ref ("<name>:<version>"), captured
         * here and used to name the stored file at FINISH_WRITE. */
        if (ref == NULL || ref[0] == '\0')
            return -EINVAL;
        strncpy(targetRef, ref, sizeof(targetRef) - 1);
        targetRef[sizeof(targetRef) - 1] = '\0';
        f = fopen(tempName, "w");
        if (f == NULL)
            return -errno;

        /* write first chunk */
        written = fwrite(buf, 1, nbytes, f);
        break;
    case CONTINUE_WRITE:
        if (buf == NULL || nbytes == 0)
            return -EINVAL;
        if (f == NULL)
            return -EBADF;
        written = fwrite(buf, 1, nbytes, f);
        break;
    case FINISH_WRITE:
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
        if (targetRef[0] == '\0') {
            remove(tempName);
            return -EINVAL;
        }

        snprintf(targetName, sizeof(targetName), "%s/%s%s", REGISTRY_ROOT,
                 targetRef, REGISTRY_EXT);
        targetRef[0] = '\0';
        if (rename(tempName, targetName) < 0) {
            remove(tempName);
            return -errno;
        }
        break;
    case ABORT_WRITE:
        if (f == NULL)
            return -EBADF;
        fclose(f);
        f = NULL;
        targetRef[0] = '\0';
        remove(tempName);
        break;
    default:
        return -EINVAL;
        break;
    }

    return written;
}

int PlatformRegistryWappLoad(const reg_entry_t *entry, wapp_t *w) {
    char targetName[PATH_MAX];
    reg_entry_t resolved;
    int ret;

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

int PlatformRegistryRemove(const reg_entry_t *entry) {
    char targetName[PATH_MAX];

    snprintf(targetName, sizeof(targetName), "%s/%s%c%s%s", REGISTRY_ROOT,
             entry->name, REGISTRY_VERSION_SEPARATOR, entry->version,
             REGISTRY_EXT);
    if (remove(targetName) != 0) {
        return -errno;
    }

    return 0;
}
