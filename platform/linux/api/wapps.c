#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>

#include <platform.h>
#include <wanted.h>
#include <wanted-api.h>

#define FATAL(msg, ...) { fprintf(stderr, "Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

typedef struct {
    pthread_t t;
    data_t data;
} thread_data_t;

struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} state;

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

int LoadWapp(const char *filename, wapp_t * wapp) {
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

int StartWapp(wapp_t *app) {

    state.threads[state.n].data.id = state.n;
    state.threads[state.n].data.wapp = app;

    pthread_create(&state.threads[state.n].t, NULL, WA_thread, (void*) &state.threads[state.n].data);
    state.n++;

    return 0;
}

void WaitForWapps() {
    for (int i = 0; i < state.n; i++) {
        pthread_join(state.threads[i].t, NULL);
    }
}



