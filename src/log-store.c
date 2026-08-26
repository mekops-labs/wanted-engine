/* SPDX-License-Identifier: Apache-2.0 */

/* Per-wapp log ring buffers — the backing store for the "log" console driver.
 * A full ring overwrites its oldest bytes rather than blocking, and reads are
 * non-destructive so the supervisor can poll a wapp's output. Keyed by name. */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <log-store.h>
#include <platform.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

typedef struct {
    char name[WAPP_MAX_NAME_LEN];
    char buf[CONFIG_WANTED_LOG_CAP];
    size_t start;  /* index of the oldest byte */
    size_t len;    /* bytes stored, <= CONFIG_WANTED_LOG_CAP */
    uint64_t tick; /* last-access counter, for LRU eviction */
    bool used;
    bool mid_line; /* a stamp is due while this is false */
} log_slot_t;

/* Nanoseconds per millisecond, the unit PlatformClockGetTime reports in. */
#define NS_PER_MS 1000000ULL

/* Written once at engine init, before any wapp thread exists, and only read
 * after. */
static uint64_t g_uptime_origin_ns;

/* Two rings in memory a reset does not clear: one this boot writes, one the
 * previous boot left. `magic` tells a first boot from a re-used region, and
 * `live` says which half is current. */
#define PERSIST_MAGIC 0x574c4f47u /* "WLOG" */

typedef struct {
    uint32_t magic;
    uint32_t live; /* 0 or 1 */
    uint32_t len[2];
} persist_hdr_t;

struct log_store_t {
    log_slot_t slots[CONFIG_WANTED_LOG_SLOTS];
    uint64_t clock; /* monotonic; stamped on each slot access */
    platform_mutex_t *lock;

    /* Null until LogStorePersistInit finds usable memory. `cap` is per half. */
    persist_hdr_t *phdr;
    char *pbuf[2];
    size_t pcap;
    bool prev_valid;
};

void LogStoreUptimeInit(void) {
    plat_timestamp_t now = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &now) == 0)
        g_uptime_origin_ns = (uint64_t)now;
}

uint64_t LogStoreUptimeMs(void) {
    plat_timestamp_t now = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &now) != 0)
        return 0;
    if ((uint64_t)now < g_uptime_origin_ns)
        return 0; /* a clock that stepped back reads as the origin */
    return ((uint64_t)now - g_uptime_origin_ns) / NS_PER_MS;
}

/* Render the line stamp; 0 when it would not fit, which drops it rather than
 * writing a partial one. */
static size_t stamp(char *out, size_t cap) {
    int n =
        snprintf(out, cap, "[+%llu] ", (unsigned long long)LogStoreUptimeMs());
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

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

/* Caller holds the lock. Find the slot for `name`, allocating on first use.
 * A full table evicts the least-recently-used wapp rather than dropping new
 * output, since the supervisor reads a wapp's log promptly after launch. */
static log_slot_t *slot_for(log_store_t *s, const char *name) {
    int free_idx = -1, lru_idx = 0;
    for (int i = 0; i < CONFIG_WANTED_LOG_SLOTS; i++) {
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

void LogStorePersistInit(log_store_t *s) {
    size_t len = 0;

    if (!s || s->phdr != NULL || CONFIG_WANTED_LOG_PERSIST_CAP == 0) {
        return;
    }

    unsigned char *mem = PlatformPersistMem(&len);
    size_t need =
        sizeof(persist_hdr_t) + ((size_t)CONFIG_WANTED_LOG_PERSIST_CAP * 2);
    if (mem == NULL || len < need) {
        return; /* no such memory here, or too little of it */
    }

    PlatformMutexLock(s->lock);
    persist_hdr_t *h = (persist_hdr_t *)mem;
    char *half0 = (char *)(mem + sizeof(persist_hdr_t));
    char *half1 = half0 + CONFIG_WANTED_LOG_PERSIST_CAP;

    if (h->magic != PERSIST_MAGIC || h->live > 1) {
        /* Nothing usable was there: a first boot, or power was lost. */
        h->magic = PERSIST_MAGIC;
        h->live = 0;
        h->len[0] = 0;
        h->len[1] = 0;
        s->prev_valid = false;
    } else {
        /* What the previous boot wrote is the half it had live. A length past
         * the cap means the region came from a build with a different cap;
         * treat it as unreadable. */
        s->prev_valid = h->len[h->live] <= CONFIG_WANTED_LOG_PERSIST_CAP;
        h->live = h->live == 0 ? 1 : 0;
        h->len[h->live] = 0;
    }

    s->phdr = h;
    s->pbuf[0] = half0;
    s->pbuf[1] = half1;
    s->pcap = CONFIG_WANTED_LOG_PERSIST_CAP;
    PlatformMutexUnlock(s->lock);

    /* Open this boot's half with why the last one ended, so the log says what
     * a reader would otherwise have to ask a separate node for. */
    char reason[24];
    if (PlatformResetReason(reason, sizeof(reason)) > 0) {
        char line[64];
        int n = snprintf(line, sizeof(line), "wanted: boot after %s\n", reason);
        if (n > 0) {
            WantedLogCapture(line, (size_t)n);
        }
    }
}

void LogStorePersistDetach(log_store_t *s) {
    if (!s) {
        return;
    }
    PlatformMutexLock(s->lock);
    s->phdr = NULL;
    s->pbuf[0] = NULL;
    s->pbuf[1] = NULL;
    s->pcap = 0;
    s->prev_valid = false;
    PlatformMutexUnlock(s->lock);
}

/* Caller holds the lock. Mirror the engine's own bytes into the live half,
 * dropping the oldest once it is full, as the RAM ring does. */
static void persistAppend(log_store_t *s, const char *p, size_t n) {
    if (s->phdr == NULL || n == 0) {
        return;
    }
    char *dst = s->pbuf[s->phdr->live];
    size_t used = s->phdr->len[s->phdr->live];

    if (n >= s->pcap) {
        p += n - s->pcap;
        n = s->pcap;
        used = 0;
    }
    if (used + n > s->pcap) {
        size_t drop = used + n - s->pcap;
        memmove(dst, dst + drop, used - drop);
        used -= drop;
    }
    memcpy(dst + used, p, n);
    s->phdr->len[s->phdr->live] = (uint32_t)(used + n);
}

/* Caller holds the lock. Drops the oldest bytes once the ring is full. */
static void slotPush(log_slot_t *sl, const char *p, size_t n) {
    for (size_t i = 0; i < n; i++) {
        sl->buf[(sl->start + sl->len) % CONFIG_WANTED_LOG_CAP] = p[i];
        if (sl->len < CONFIG_WANTED_LOG_CAP)
            sl->len++;
        else
            sl->start = (sl->start + 1) % CONFIG_WANTED_LOG_CAP;
    }
}

void LogStoreAppend(log_store_t *s, const char *name, const void *buf,
                    size_t n) {
    if (!s || !name || !buf || n == 0)
        return;

    PlatformMutexLock(s->lock);
    /* The engine's own channel is mirrored where a reset cannot clear it.
     * strncmp stops at the literal's own NUL well inside WAPP_MAX_NAME_LEN. */
    /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    bool engine = strncmp(name, WANTED_ENGINE_LOG_NAME, WAPP_MAX_NAME_LEN) == 0;
    log_slot_t *sl = slot_for(s, name);
    if (sl == NULL) {
        PlatformMutexUnlock(s->lock);
        return;
    }

    /* A write carries whatever the producer flushed, so a line is closed by a
     * newline and not by the call ending. */
    const char *p = (const char *)buf;
    size_t i = 0;
    while (i < n) {
        if (!sl->mid_line) {
            char ts[LOG_STAMP_MAX];
            size_t m = stamp(ts, sizeof(ts));
            slotPush(sl, ts, m);
            if (engine)
                persistAppend(s, ts, m);
            sl->mid_line = true;
        }
        size_t run = i;
        while (run < n && p[run] != '\n')
            run++;
        if (run < n) {
            run++;
            sl->mid_line = false;
        }
        slotPush(sl, p + i, run - i);
        if (engine)
            persistAppend(s, p + i, run - i);
        i = run;
    }
    PlatformMutexUnlock(s->lock);
}

size_t LogStoreRead(log_store_t *s, const char *name, char *out, size_t cap) {
    if (!s || !name || !out || cap == 0)
        return 0;

    PlatformMutexLock(s->lock);
    /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    if (strncmp(name, WANTED_PREV_LOG_NAME, WAPP_MAX_NAME_LEN) == 0) {
        size_t copied = 0;
        if (s->prev_valid && s->phdr != NULL) {
            uint32_t other = s->phdr->live == 0 ? 1 : 0;
            size_t have = s->phdr->len[other];
            copied = have < cap ? have : cap;
            memcpy(out, s->pbuf[other], copied);
        }
        PlatformMutexUnlock(s->lock);
        return copied;
    }
    size_t copied = 0;
    for (int i = 0; i < CONFIG_WANTED_LOG_SLOTS; i++) {
        if (s->slots[i].used &&
            strncmp(s->slots[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            log_slot_t *sl = &s->slots[i];
            sl->tick = ++s->clock; /* a read counts as recent use */
            size_t m = sl->len < cap ? sl->len : cap;
            for (size_t j = 0; j < m; j++)
                out[j] = sl->buf[(sl->start + j) % CONFIG_WANTED_LOG_CAP];
            copied = m;
            break;
        }
    }
    PlatformMutexUnlock(s->lock);
    return copied;
}

bool LogStoreHas(log_store_t *s, const char *name) {
    if (!s || !name)
        return false;

    PlatformMutexLock(s->lock);
    /* NOLINTNEXTLINE(bugprone-not-null-terminated-result) */
    if (strncmp(name, WANTED_PREV_LOG_NAME, WAPP_MAX_NAME_LEN) == 0) {
        bool present = s->prev_valid && s->phdr != NULL;
        PlatformMutexUnlock(s->lock);
        return present;
    }
    bool found = false;
    for (int i = 0; i < CONFIG_WANTED_LOG_SLOTS; i++) {
        if (s->slots[i].used &&
            strncmp(s->slots[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            found = true;
            break;
        }
    }
    PlatformMutexUnlock(s->lock);
    return found;
}

size_t LogStoreList(log_store_t *s, char names[][WAPP_MAX_NAME_LEN],
                    size_t max) {
    if (!s || !names || max == 0)
        return 0;

    PlatformMutexLock(s->lock);
    size_t n = 0;
    if (s->prev_valid && s->phdr != NULL) {
        strncpy(names[n], WANTED_PREV_LOG_NAME, WAPP_MAX_NAME_LEN - 1);
        names[n][WAPP_MAX_NAME_LEN - 1] = '\0';
        n++;
    }
    for (int i = 0; i < CONFIG_WANTED_LOG_SLOTS && n < max; i++) {
        if (!s->slots[i].used)
            continue;
        strncpy(names[n], s->slots[i].name, WAPP_MAX_NAME_LEN - 1);
        names[n][WAPP_MAX_NAME_LEN - 1] = '\0';
        n++;
    }
    PlatformMutexUnlock(s->lock);
    return n;
}

/* The engine's error channel, captured under a name no wapp image can claim.
 * A store that failed to allocate makes this a no-op, as it does for a wapp. */
void WantedLogCapture(const void *buf, size_t n) {
    LogStoreAppend(LogStore(), WANTED_ENGINE_LOG_NAME, buf, n);
}
