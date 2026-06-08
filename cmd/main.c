#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wanted.h>

#define STR(...) #__VA_ARGS__

char *defCfg = STR({"system" : {}});

int main(int argc, char *argv[]) {
    int ret;
    char *cfg;

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
            perror(argv[0]);
            return -errno;
        }

        r = fread(cfg, 1, sz, fp);
        if (r != sz) {
            fprintf(stderr, "can't read config file\n");
            return -1;
        }

        fclose(fp);
    } else {
        cfg = defCfg;
    }

    ret = WantedStart(cfg, strlen(cfg));
    if (ret < 0) {
        errno = -ret;
        perror("config parse error");
        return ret;
    }

    printf("\nAll wapps ended, done...\n");

    if (argc > 1) {
        free(cfg);
    }

    return 0;
}
