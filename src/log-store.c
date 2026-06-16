/* SPDX-License-Identifier: Apache-2.0 */

/* Per-wapp log ring buffers — the backing store for the "log" console driver.
 *
 * Structurally a sibling of the named-pipe store (vfs-pipe.c): a process-wide
 * set of fixed ring buffers guarded by one mutex. Two differences suit a log
 * rather than a pipe: a full ring overwrites its oldest bytes (keep the most
 * recent output) instead of blocking, and reads are non-destructive so the
 * supervisor can poll a wapp's output repeatedly. Keyed by wapp name. */

#include <stdint.h>
#include <string.h>

#include <log-store.h>
#include <platform.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

#define LOG_CAP 2048 /* bytes retained per wapp (most recent) */

typedef struct {
    char name[WAPP_MAX_NAME_LEN];
    char buf[LOG_CAP];
    size_t start;  /* index of the oldest byte */
    size_t len;    /* bytes stored, <= LOG_CAP */
    uint64_t tick; /* last-access counter, for LRU eviction */
    bool used;
} log_slot_t;

struct log_store_t {
    log_slot_t slots[LOG_SLOTS];
    uint64_t clock; /* monotonic; stamped on each slot access */
    platform_mutex_t *lock;
};

log_store_t *LogStore(void) {
    static log_store_t *store = NULL;
    if (!store) {
        store = WantedMalloc(sizeof(*store));
        if (!store)
            return NULL;
        memset(store, 0, sizeof(*store));
        store->lock = PlatformMutexNew(); /* NULL-tolerant downstream */
    }
    return store;
}

/* Caller holds the lock. Find the slot for `name`, allocating one on first use.
 * When the table is full, evict the least-recently-used wapp rather than drop
 * the new output: the store keeps only the most recent activity (it already
 * overwrites the oldest bytes within a slot), and the supervisor reads a wapp's
 * log promptly after launching it, so the live wapps are always retained. */
static log_slot_t *slot_for(log_store_t *s, const char *name) {
    int free_idx = -1, lru_idx = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (s->slots[i].used) {
            if (strncmp(s->slots[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
                s->slots[i].tick = ++s->clock;
                return &s->slots[i];
            }
            if (s->slots[i].tick < s->slots[lru_idx].tick)
                lru_idx = i;
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    log_slot_t *sl = &s->slots[free_idx >= 0 ? free_idx : lru_idx];
    memset(sl, 0, sizeof(*sl));
    strncpy(sl->name, name, WAPP_MAX_NAME_LEN - 1);
    sl->name[WAPP_MAX_NAME_LEN - 1] = '\0';
    sl->tick = ++s->clock;
    sl->used = true;
    return sl;
}

void LogStoreAppend(log_store_t *s, const char *name, const void *buf,
                    size_t n) {
    if (!s || !name || !buf || n == 0)
        return;

    PlatformMutexLock(s->lock);
    log_slot_t *sl = slot_for(s, name);
    if (sl) {
        const char *p = (const char *)buf;
        /* Only the last LOG_CAP bytes can survive. */
        if (n >= LOG_CAP) {
            p += n - LOG_CAP;
            n = LOG_CAP;
        }
        for (size_t i = 0; i < n; i++) {
            sl->buf[(sl->start + sl->len) % LOG_CAP] = p[i];
            if (sl->len < LOG_CAP)
                sl->len++;
            else
                sl->start = (sl->start + 1) % LOG_CAP; /* overwrite oldest */
        }
    }
    PlatformMutexUnlock(s->lock);
}

size_t LogStoreRead(log_store_t *s, const char *name, char *out, size_t cap) {
    if (!s || !name || !out || cap == 0)
        return 0;

    PlatformMutexLock(s->lock);
    size_t copied = 0;
    for (int i = 0; i < LOG_SLOTS; i++) {
        if (s->slots[i].used &&
            strncmp(s->slots[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            log_slot_t *sl = &s->slots[i];
            sl->tick = ++s->clock; /* a read counts as recent use */
            size_t m = sl->len < cap ? sl->len : cap;
            for (size_t j = 0; j < m; j++)
                out[j] = sl->buf[(sl->start + j) % LOG_CAP];
            copied = m;
            break;
        }
    }
    PlatformMutexUnlock(s->lock);
    return copied;
}
