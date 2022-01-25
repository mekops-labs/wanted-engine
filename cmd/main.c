#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>

#include <wanted.h>

#define FATAL(msg, ...) { fprintf(stderr, "Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

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

    wapp->img = img;
    wapp->img_len = filesize;

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

    if (argc < 2) {
        FATAL("Please provide at least one image file name");
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

        pthread_create( &config.threads[i].t, NULL, WA_thread, (void*) &config.threads[i].data);
    }

    for (int i = 0; i < config.n; i++) {
        pthread_join( config.threads[i].t, NULL);
    }

    return 0;
}
