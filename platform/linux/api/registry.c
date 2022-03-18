#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>

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


int PlatformReadRegistry(reg_entry_t *registryList, size_t len)
{
    struct dirent **namelist;
    struct stat s;
    int n, i = 0;
    int ret;
    int d;

    d = open(REGISTRY_ROOT, O_DIRECTORY | O_RDONLY);
    if (d < 0) {
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

    return n;
}
