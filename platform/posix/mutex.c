/* SPDX-License-Identifier: Apache-2.0 */

/* pthread-backed platform mutex. Wraps the opaque platform_mutex_t so src/
 * shared state can serialise access without including <pthread.h> itself.
 * Shared by every POSIX platform target (Linux and the NuttX POSIX layer). */

#include <pthread.h>

#include <platform.h>
#include <wanted_malloc.h>

struct platform_mutex_t {
    pthread_mutex_t mtx;
};

platform_mutex_t *PlatformMutexNew(void) {
    platform_mutex_t *m = WantedMalloc(sizeof(*m));
    if (!m)
        return NULL;
    if (pthread_mutex_init(&m->mtx, NULL) != 0) {
        WantedFree(m);
        return NULL;
    }
    return m;
}

void PlatformMutexLock(platform_mutex_t *m) {
    if (m)
        pthread_mutex_lock(&m->mtx);
}

void PlatformMutexUnlock(platform_mutex_t *m) {
    if (m)
        pthread_mutex_unlock(&m->mtx);
}

void PlatformMutexFree(platform_mutex_t *m) {
    if (m) {
        pthread_mutex_destroy(&m->mtx);
        WantedFree(m);
    }
}
