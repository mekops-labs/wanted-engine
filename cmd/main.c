/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <platform.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wanted.h>

#define STR(...) #__VA_ARGS__

char *defCfg = STR({"system" : {}});

int main(int argc, char *argv[]) {
    int ret;
    char *cfg;

    /* Hand argv to the platform so a reboot request can re-exec this image. */
    PlatformSetProcessArgs(argc, argv);

    if (argc > 1) {
        long sz;
        size_t r;
        FILE *fp = fopen(argv[1], "r");
        if (!fp) {
            perror(argv[1]);
            return -errno;
        }

        fseek(fp, 0L, SEEK_END);
        sz = ftell(fp);
        fseek(fp, 0L, SEEK_SET);
        if (sz < 0) {
            fprintf(stderr, "can't determine config file size\n");
            fclose(fp);
            return -EIO;
        }

        /* One extra byte for the NUL terminator: the config is consumed as a
         * C string (strlen below), so the buffer must be terminated. */
        cfg = (char *)malloc(sz + 1);
        if (!cfg) {
            perror(argv[0]);
            ret = -errno;
            fclose(fp);
            return ret;
        }

        r = fread(cfg, 1, sz, fp);
        if (r != sz) {
            fprintf(stderr, "can't read config file\n");
            free(cfg);
            fclose(fp);
            return -1;
        }
        cfg[sz] = '\0';

        fclose(fp);
    } else {
        cfg = defCfg;
    }

    ret = WantedStart(cfg, strlen(cfg));

    if (argc > 1)
        free(cfg);

    if (ret < 0) {
        errno = -ret;
        perror("config parse error");
        return ret;
    }

    printf("\nAll wapps ended, done...\n");

    return 0;
}
