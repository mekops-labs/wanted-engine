/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX built-in application entry point.
 *
 * Mirrors cmd/main.c: load a JSON config from argv[1] when given, otherwise use
 * the compiled-in default, then run the engine via WantedStart. NuttX builds no
 * standalone cmd/ executable; this is registered as the "wanted" built-in app. */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <platform.h>
#include <wanted.h>

/* FAR is a NuttX pointer qualifier (empty on flat-memory targets such as the
 * sim). Define it away for the host scaffolding build, which lacks it. */
#ifndef FAR
#define FAR
#endif

#define STR(...) #__VA_ARGS__

static char *defCfg = STR({"system" : {}});

int wanted_main(int argc, FAR char *argv[]) {
    int ret;
    char *cfg;
    size_t cfgLen;
    bool allocated = false;

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
        rewind(fp);

        cfg = (char *)malloc(sz);
        if (!cfg) {
            fclose(fp);
            perror(argv[0]);
            return -errno;
        }

        r = fread(cfg, 1, sz, fp);
        fclose(fp);
        if (r != (size_t)sz) {
            fprintf(stderr, "can't read config file\n");
            free(cfg);
            return -1;
        }

        cfgLen = (size_t)sz;
        allocated = true;
    } else {
        cfg = defCfg;
        cfgLen = strlen(defCfg);
    }

    ret = WantedStart(cfg, cfgLen);

    if (allocated) {
        free(cfg);
    }

    if (ret < 0) {
        errno = -ret;
        perror("config parse error");
        return ret;
    }

    printf("\nAll wapps ended, done...\n");

    return 0;
}
