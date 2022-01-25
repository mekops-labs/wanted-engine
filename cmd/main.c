#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <unistd.h>
#include <pthread.h>

#include <sys/mman.h>

#include <wanted.h>

#ifdef WANTED_ROMFS
#include <romfs.h>

romfsimg_t romfs;
#endif

#define FATAL(msg, ...) { printf("Fatal: " msg "\n", ##__VA_ARGS__); return -1; }
#define MAX_WAPPS 3

typedef struct {
    pthread_t t;
    data_t data;
} thread_data_t;

struct {
    wapp_t wapps[MAX_WAPPS];
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} config;

int loadWapp(const char *filename, wapp_t * wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;

    f = fopen(filename, "rb");
    if (NULL == f) FATAL("can't open %s", filename);

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED) FATAL("can't map file");

#ifndef WANTED_ROMFS
    wapp->wasm = img;
    wapp->wasm_len = filesize;
#else
    int ret;
    char *wasmName = "app.wasm";

    romfs.img = img;
    romfs.len = filesize;

    ret = RomfsLoad(img, filesize);
    if (ret < 0) FATAL("load returned %d", ret);

    ret = RomfsFdStatAt(3, wasmName, NULL);
    if (ret < 0) FATAL("stat returned %d", ret);
    if (!IS_FILE(ret)) FATAL("%s is not correct file", wasmName);

    ret = RomfsOpenAt(3, wasmName, 0);
    if (ret < 0) FATAL("open returned %d", ret);

    ret = RomfsMapFile((void **)&wapp->wasm, &wapp->wasm_len, ret, 0);

    RomfsClose(ret);
#endif

    fclose(f);
    return 0;
}

void *WA_thread( void *ptr )
{
    RunWapp((data_t *)ptr);
    return NULL;
}

int main(int argc, char* argv[]) {
    wapp_t wapp;
    char *defaultFile = "app.romfs";

    if (argc < 2) {
        if (loadWapp(defaultFile, &config.wapps[0]) < 0) FATAL("Wapp loading failed");
        config.n = 1;
    } else {
        for (int i = 0; i < argc - 1; i++) {
            if (loadWapp(argv[i+1], &config.wapps[i]) < 0) FATAL("Wapp loading failed");
        }
        config.n = argc - 1;
    }

    MyApiInit();

    for (int i = 0; i < config.n; i++) {
        config.threads[i].data.id   = i;
        config.threads[i].data.wapp = &config.wapps[i];
#ifdef WANTED_ROMFS
        config.threads[i].data.romfs = romfs;
#endif

        pthread_create( &config.threads[i].t, NULL, WA_thread, (void*) &config.threads[i].data);
    }

    for (int i = 0; i < config.n; i++) {
        pthread_join( config.threads[i].t, NULL);
    }

    return 0;
}
