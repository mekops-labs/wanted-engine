#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <config-linux.h>
#include <platform.h>
#include <wanted-api.h>
#include <wanted.h>

#include <debug_trace.h>

#ifdef __ANDROID__
#define GNU_SOURCE
#include <signal.h>
#endif

pthread_mutex_t state_mtx = PTHREAD_MUTEX_INITIALIZER;

#define FATAL(err, msg, ...)                                                   \
    {                                                                          \
        DEBUG_TRACE("Fatal: " msg, ##__VA_ARGS__);                             \
        return err;                                                            \
    }

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

void WA_threadEnd(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

    WantedWappStop(d);

    updateState(d->id, d->lastStatus);
}

#ifdef __ANDROID__
void thread_sigHandler(int sig) {
    int i;
    if (sig != SIGUSR1) {
        return;
    }

    pthread_t t = pthread_self();

    for (i = 0; i < MAX_WAPPS; i++) {
        if (state.threads[i].t == t)
            break;
    }
    if (i == MAX_WAPPS)
        return;

    WA_threadEnd((void *)&state.threads[i].data);

    pthread_exit(NULL);
}
#endif

void *WA_thread(void *ptr) {
    wapp_data_t *d = (wapp_data_t *)ptr;

#ifndef __ANDROID__
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
#else
    struct sigaction actions;
    memset(&actions, 0, sizeof(actions));
    sigemptyset(&actions.sa_mask);
    actions.sa_flags = 0;
    actions.sa_handler = thread_sigHandler;
    sigaction(SIGUSR1, &actions, NULL);
#endif

    pthread_cleanup_push(WA_threadEnd, ptr);

    pthread_mutex_lock(&state_mtx);
    state.threads[d->id].status = RUNNING;
    pthread_mutex_unlock(&state_mtx);

    d->lastStatus = 0;
    d->lastStatus = WantedWappRun(d);

    pthread_cleanup_pop(1);

    pthread_exit(NULL);
}

int PlatformWappLoad(const char *path, wapp_t *wapp) {
    long filesize;
    FILE *f;
    uint8_t *img;

    if (NULL == wapp) {
        return -EINVAL;
    }

    DEBUG_TRACE("Opening: %s\n", path);

    f = fopen(path, "rb");

    if (NULL == f) {
        FATAL(-errno, "can't open wapp: %s", path);
    }

    fseek(f, 0L, SEEK_END);
    filesize = ftell(f);
    rewind(f);

    img = (uint8_t *)mmap(NULL, filesize, PROT_READ, MAP_PRIVATE, fileno(f), 0);
    if (img == MAP_FAILED)
        FATAL(-errno, "can't map file");

    wapp->layers[0] = img;
    wapp->layer_lens[0] = filesize;
    wapp->layer_cnt = 1;

    fclose(f);
    return 0;
}

int PlatformWappUnload(const wapp_t *wapp) {
    if (NULL == wapp) {
        return -EINVAL;
    }

    if (munmap(wapp->layers[0], wapp->layer_lens[0]) < 0)
        FATAL(-errno, "can't unmap file");
    return 0;
}

int PlatformWappStart(wapp_t *wapp) {
    int slot;

    if (NULL == wapp) {
        return -EINVAL;
    }

    pthread_mutex_lock(&state_mtx);
    if (state.n >= MAX_WAPPS) {
        pthread_mutex_unlock(&state_mtx);
        return -ENOSPC;
    }

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].status == NOT_STARTED ||
            state.threads[slot].status == EXITED ||
            state.threads[slot].status == FAILURE)
            break;
    }

    state.threads[slot].data.id = slot;
    state.threads[slot].data.wapp = wapp;
    state.threads[slot].status = STARTING;

    pthread_create((pthread_t *)&state.threads[slot].t, NULL, WA_thread,
                   (void *)&state.threads[slot].data);
    pthread_detach(state.threads[slot].t);
    state.n++;

    pthread_mutex_unlock(&state_mtx);

    return 0;
}

int PlatformWappStop(const char *name) {
    int slot;

    for (slot = 0; slot < MAX_WAPPS; slot++) {
        if (state.threads[slot].data.wapp == NULL)
            continue;
        if ((strcmp((char *)state.threads[slot].data.wapp->name, name) == 0) &&
            state.threads[slot].status == RUNNING)
            break;
    }

    if (slot == MAX_WAPPS) {
        return -ENOENT;
    }

#ifndef __ANDROID__
    return -pthread_cancel(state.threads[slot].t);
#else
    return -pthread_kill(state.threads[slot].t, SIGUSR1);
#endif
}

void PlatformWappLoop() {
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
            if (state.threads[i].data.wapp == NULL)
                continue;

            if (strncmp((const char *)state.threads[i].data.wapp->name,
                        "supervisor", strlen("supervisor")) == 0 &&
                state.threads[i].status == RUNNING) {
                supervisorOk++;
            }
        }

        if (!supervisorOk) {
            PlatformWappStart(WantedGetCurrentSupervisor());
        }
    }
}

int PlatformWappGetState(wapp_state_t *wapps, size_t appsLen) {
    int i, r;

    for (i = 0, r = 0; i < MAX_WAPPS && r < appsLen; i++) {
        if (state.threads[i].data.wapp == NULL)
            continue;

        strncpy(wapps[r].name, (const char *)state.threads[i].data.wapp->name,
                WAPP_MAX_NAME_LEN);
        wapps[r].name[WAPP_MAX_NAME_LEN - 1] = '\0';
        wapps[r].status = state.threads[i].status;
        wapps[r].version = state.threads[i].data.wapp->version;
        wapps[r].id = state.threads[i].data.id;
        r++;
    }

    return r;
}

#include <malloc.h>
void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    struct mallinfo2 mi = mallinfo2();
    if (heap_used)  *heap_used  = mi.uordblks;
    if (heap_total) *heap_total = mi.arena;
}
