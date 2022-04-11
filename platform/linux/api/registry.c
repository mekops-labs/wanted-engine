#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <platform.h>
#include <config-linux.h>

static inline size_t min(size_t a, size_t b) {
    return (a) > (b) ? (b) : (a);
}

/* when return 1, scandir will put this dirent to the list */
static int ParseExt(const struct dirent *dir)
{
    if(!dir) {
        return 0;
    }

    if(dir->d_type == DT_REG) { /* only deal with regular file */
        const char *ext = strrchr(dir->d_name,'.');
        if((!ext) || (ext == dir->d_name)) {
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
     if((!ext) || (ext == name)) {
        return 0;
     }
     return (int)(ext - name);
}


int PlatformRegistryRead(reg_entry_t *registryList, size_t len)
{
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

    n = scandirat(d ,".", &namelist, ParseExt, alphasort);
    if (n < 0) {
        return -errno;
    }

    for (i = 0; i < n; i++, len--) {
        if (registryList != NULL) {
            if (len == 0) {
                break;
            }
            size_t entryNameLen = min(NameLenWithoutExt(namelist[i]->d_name)+1, WAPP_MAX_NAME_LEN);

            strncpy(
                registryList[i].name,
                namelist[i]->d_name,
                entryNameLen
                );

            registryList[i].name[entryNameLen-1] = '\0';

            ret = fstatat(d, namelist[i]->d_name, &s, 0);
            registryList[i].size = s.st_size;
        }
        free(namelist[i]);
    }

    free(namelist);

    close(d);

    return n;
}

int PlatformRegistryWrite(write_state_t s, const uint8_t *buf, size_t nbytes)
{
    static FILE *f;
    static char tempName[NAME_MAX];
    static char targetName[NAME_MAX];

    int written = 0;
    int ret = 0;
    wapp_t w;

    switch (s)
    {
    case START_WRITE:
        if (buf == NULL || nbytes == 0) return -EINVAL;
        snprintf(tempName, NAME_MAX, "%s/%s%s", REGISTRY_ROOT, "_temp", REGISTRY_EXT);
        f = fopen(tempName, "w");
        if (f == NULL) return -errno;

        /* write first chunk */
        written = fwrite(buf, 1, nbytes, f);
        break;
    case CONTINUE_WRITE:
        if (buf == NULL || nbytes == 0) return -EINVAL;
        if (f == NULL) return -EBADF;
        written = fwrite(buf, 1, nbytes, f);
        break;
    case FINISH_WRITE:
        if (f == NULL) return -EBADF;
        fclose(f);
        f = NULL;

        ret = PlatformWappLoad("_temp", &w);
        if (ret < 0) {
            remove(tempName);
            return ret;
        }

        ret = WantedWappParseManifest(&w);
        if (ret < 0) {
            remove(tempName);
            return ret;
        }

        snprintf(targetName, NAME_MAX, "%s/%s%s", REGISTRY_ROOT, w.name, REGISTRY_EXT);
        if (rename(tempName, targetName) < 0) {
            remove(tempName);
            written = -errno;
        }

        ret = PlatformWappUnload(&w);
        if (ret < 0) return ret;

        break;
    case ABORT_WRITE:
        if (f == NULL) return -EBADF;
        fclose(f);
        f = NULL;
        remove(tempName);
        break;
    default:
        return -EINVAL;
        break;
    }

    return written;
}

int PlatformRegistryRemove(const char *name)
{
    char targetName[NAME_MAX];

    snprintf(targetName, NAME_MAX, "%s/%s%s", REGISTRY_ROOT, name, REGISTRY_EXT);
    if (remove(targetName) != 0) {
        return -errno;
    }

    return 0;
}
