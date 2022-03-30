#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>

#include <platform.h>
#include <wanted.h>
#include <wanted-api.h>
#include <config-linux.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

#define FATAL(msg, ...) { fprintf(stderr, "Fatal: " msg "\n", ##__VA_ARGS__); return -1; }

typedef struct {
    pthread_t t;
    status_t status;
    wapp_data_t data;
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} state;

void WA_threadEnd(void *ptr)
{
    StopWapp((wapp_data_t *)ptr);
}

void *WA_thread(void *ptr)
{
    int ret;
    wapp_data_t *d = (wapp_data_t *)ptr;

    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
    pthread_cleanup_push(WA_threadEnd, ptr);

    pthread_mutex_lock(&state_mtx);
    state.threads[d->id].status = RUNNING;
    pthread_mutex_unlock(&state_mtx);

    ret = RunWapp(d);

    pthread_cleanup_pop(0);

    pthread_mutex_lock(&state_mtx);
    if (ret == 0) {
        state.threads[d->id].status = EXITED;
    } else {
        state.threads[d->id].status = FAILURE;
    }
    state.n--;
    pthread_mutex_unlock(&state_mtx);

    pthread_exit(NULL);
}

int LoadWapp(const char *name, wapp_t * wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;
    size_t filenameLen = strlen(REGISTRY_ROOT) + 1 + strlen(name) + strlen(REGISTRY_EXT) + 1;
    char *filename = malloc(filenameLen);

    snprintf(filename, filenameLen, "%s/%s%s", REGISTRY_ROOT, name, REGISTRY_EXT);

    printf("Opening: %s\n", filename);

    f = fopen(filename, "rb");
    free(filename);
    if (NULL == f) {
        FATAL("can't open %s", filename);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED) FATAL("can't map file");

    wapp->img = img;
    wapp->img_len = filesize;
    strncpy(wapp->name, name, WAPP_MAX_NAME_LEN);
    wapp->name[WAPP_MAX_NAME_LEN-1] = 0;

    /* TODO: read version from romfs */
    wapp->version.major = 1;
    wapp->version.minor = 0;
    wapp->version.patch = 0;

    fclose(f);
    return 0;
}

int StartWapp(wapp_t app) {
    int slot;

    pthread_mutex_lock(&state_mtx);
    if (state.n >= MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].status == NOT_STARTED || state.threads[slot].status == EXITED || state.threads[slot].status == FAILURE)
            break;
    }

    state.threads[slot].data.id = slot;
    state.threads[slot].data.wapp = app;
    state.threads[slot].status = STARTING;

    pthread_create((pthread_t *)&state.threads[slot].t, NULL, WA_thread, (void*) &state.threads[slot].data);
    pthread_detach(state.threads[slot].t);
    state.n++;

    pthread_mutex_unlock(&state_mtx);

    return 0;
}

void WaitForWapps() {
    for (;;) {
        if (!state.n) {
            return;
        }
    }
}

int  GetState(wapp_state_t *wapps, size_t appsLen)
{
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
            if (state.threads[i].data.wapp.img == NULL) continue;

            strncpy(wapps[r].name, (const char *)state.threads[i].data.wapp.name, WAPP_MAX_NAME_LEN);
            wapps[r].name[WAPP_MAX_NAME_LEN-1] = '\0';
            wapps[r].status = state.threads[i].status;
            wapps[r].version = state.threads[i].data.wapp.version;
            r++;
    }

    return r;
}


