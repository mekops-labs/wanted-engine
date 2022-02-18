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

    printf("Opening: %s\n", filename);

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

void WA_threadEnd(void *ptr)
{
    StopWapp((data_t *)ptr);
}

void *WA_thread(void *ptr)
{
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(WA_threadEnd, ptr);

    RunWapp((data_t *)ptr);

    pthread_cleanup_pop(0);

    pthread_exit(NULL);
}

int main(int argc, char* argv[])
{
    wapp_t wapp;
    const char *defFile = "default";

    if (argc < 2) {
        //FATAL("Please provide at least one image file name");
        if (loadWapp(defFile, &config.wapps[0]) < 0) FATAL("Wapp loading failed");
        config.n = 1;
    } else {
        for (int i = 0; i < argc - 1; i++) {
            if (loadWapp(argv[i+1], &config.wapps[i]) < 0) FATAL("Wapp loading failed");
        }
        config.n = argc - 1;
    }

    for (int i = 0; i < config.n; i++) {
        config.threads[i].data.id   = i;
        config.threads[i].data.wapp = &config.wapps[i];

        pthread_create( &config.threads[i].t, NULL, WA_thread, (void*) &config.threads[i].data);
    }

    sleep(3);
    pthread_cancel(config.threads[0].t);
    sleep(1);

    pthread_create( &config.threads[0].t, NULL, WA_thread, (void*) &config.threads[0].data);

    for (int i = 0; i < config.n; i++) {
        pthread_join( config.threads[i].t, NULL);
    }

    return 0;
}
