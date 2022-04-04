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

#include <debug_trace.h>

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

#define FATAL(err, msg, ...) { DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__); return err; }

typedef struct {
    pthread_t t;
    status_t status;
    wapp_data_t data;
} thread_data_t;

volatile struct {
    size_t n;
    thread_data_t threads[MAX_WAPPS];
} state;

static void updateState(uint8_t id, int ret) {
    pthread_mutex_lock(&state_mtx);
    if (ret == 0) {
        state.threads[id].status = EXITED;
    } else {
        state.threads[id].status = FAILURE;
    }
    state.n--;
    pthread_mutex_unlock(&state_mtx);
}

void WA_threadEnd(void *ptr)
{
    wapp_data_t *d = (wapp_data_t *)ptr;

    WantedWappStop(d);

    updateState(d->id, d->lastStatus);
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

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    pthread_cleanup_pop(1);

    pthread_exit(NULL);
}

int PlatformWappLoad(const char *name, wapp_t * wapp)
{
    long filesize;
    FILE *f;
    uint8_t *img;
    size_t filenameLen = strlen(REGISTRY_ROOT) + 1 + strlen(name) + strlen(REGISTRY_EXT) + 1;
    char *filename = malloc(filenameLen);

    snprintf(filename, filenameLen, "%s/%s%s", REGISTRY_ROOT, name, REGISTRY_EXT);

    DEBUG_TRACE("Opening: %s\n", filename);

    f = fopen(filename, "rb");
    free(filename);

    if (NULL == f) {
        FATAL(-errno, "can't open wapp: %s", name);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED)
        FATAL(-errno, "can't map file");

    wapp->img = img;
    wapp->img_len = filesize;
    strncpy(wapp->name, name, WAPP_MAX_NAME_LEN);
    memset(wapp->version.v, -1, 3);

    fclose(f);
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp)
{
    if (munmap(wapp->img, wapp->img_len) < 0)
        FATAL(-errno, "can't unmap file");
    return 0;
}

int PlatformWappStart(wapp_t app)
{
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

int PlatformWappStop(uint8_t id)
{
    int slot;

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].data.id == id &&
            state.threads[slot].status == RUNNING) break;
    }

    if (slot == MAX_WAPPS) {
        return -ENOENT;
    }

    return pthread_cancel(state.threads[slot].t);
}

void PlatformWappLoop()
{
    uint8_t supervisorOk;

    for (;;) {
        sleep(1);

        if (!state.n) {
            /* when only supervisor was running and it ended, let's exit */
            /* TODO: maybe this needs to be removed */
            return;
        }

        supervisorOk = 0;
        for (int i = 0; i < MAX_WAPPS; i++) {
            /* at least 1 supervisor needs to be running */
            if (strncmp((const char*)state.threads[i].data.wapp.name, "supervisor", strlen("supervisor")) == 0 &&
                state.threads[i].status == RUNNING) {
                supervisorOk++;
            }
        }

        if (!supervisorOk) {
            PlatformWappStart(WantedGetCurrentSupervisor());
        }
    }
}

int  PlatformWappGetState(wapp_state_t *wapps, size_t appsLen)
{
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
            if (state.threads[i].data.wapp.img == NULL) continue;

            strncpy(wapps[r].name, (const char *)state.threads[i].data.wapp.name, WAPP_MAX_NAME_LEN);
            wapps[r].name[WAPP_MAX_NAME_LEN-1] = '\0';
            wapps[r].status = state.threads[i].status;
            wapps[r].version = state.threads[i].data.wapp.version;
            wapps[r].id = state.threads[i].data.id;
            r++;
    }

    return r;
}


