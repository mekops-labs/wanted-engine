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

static inline size_t min(size_t a, size_t b) { return (a) > (b) ? (b) : (a); }

/* when return 1, scandir will put this dirent to the list */
static int ParseExt(const struct dirent *dir) {
    if (!dir) {
        return 0;
    }

    if (dir->d_type == DT_REG) { /* only deal with regular file */
        const char *ext = strrchr(dir->d_name, '.');
        if ((!ext) || (ext == dir->d_name)) {
            return 0;
        } else {
            if (strcmp(ext, REGISTRY_EXT) == 0) {
                return 1;
            }
        }
    }

    return 0;
}

static int NameLenWithoutExt(const char *name) {
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
    int ret;
    int d;

    d = open(REGISTRY_ROOT, O_DIRECTORY | O_RDONLY);
    if (d < 0) {
        if (ENOENT == errno) {
            ret = mkdir(REGISTRY_ROOT, 0755);
            if (ret < 0) {
                return -errno;
            }

            return 0;
        }
        return -errno;
    }

    n = scandir(REGISTRY_ROOT, &namelist, ParseExt, alphasort);
    if (n < 0) {
        return -errno;
    }

    for (i = 0; i < n; i++, len--) {
        if (registryList != NULL && len > 0) {
            size_t entryNameLen =
                min(NameLenWithoutExt(namelist[i]->d_name) + 1,
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

            ret = fstatat(d, namelist[i]->d_name, &s, 0);
            registryList[i].size = s.st_size;
        }

        free(namelist[i]);
    }

    free(namelist);

    close(d);

    return i;
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
        /* Resolve a bare name to the first registry entry that carries it. */
        int num = PlatformRegistryRead(NULL, 0);
        if (num < 0)
            return num;

        reg_entry_t list[num];
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

    /* Image identity is the registry entry — name and version. The instance
     * name (w->name) is set by the launch path and left untouched here. */
    strncpy(w->image, resolved.name, WAPP_MAX_NAME_LEN - 1);
    w->image[WAPP_MAX_NAME_LEN - 1] = '\0';
    return ParseVersionString(resolved.version, &w->version);
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
