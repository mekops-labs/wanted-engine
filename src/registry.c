/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <registry.h>
#include <wanted-vfs-api.h>
#include <wanted_log.h>

#define MAX_STORES REGISTRY_MAX_STORES
#define MAX_PENDING ((size_t)2 * REGISTRY_META_MAX_LAYERS)
#define MAX_MAPS                                                               \
    ((size_t)(CONFIG_WANTED_MAX_WAPPS + 1) * REGISTRY_META_MAX_LAYERS)
#define MAX_BLOBS 64
#define MAX_RESIDENT 16
#define NS_PER_SEC 1000000000ULL

typedef struct {
    bool used;
    registry_store_t *store; /* NULL for a resident image */
    registry_blob_map_t map;
    uint8_t digest[REGISTRY_DIGEST_LEN];
} map_t;

typedef struct {
    bool used;
    registry_key_t key;
    const uint8_t *data;
    size_t len;
    int64_t created;
    uint8_t digest[REGISTRY_DIGEST_LEN];
} resident_t;

typedef struct {
    bool used;
    registry_key_t key;
    uint8_t index;
    uint8_t digest[REGISTRY_DIGEST_LEN];
    uint32_t len;
    uint32_t order;
} pending_t;

typedef struct {
    bool active;
    bool plain; /* the plain route also commits at FINISH_WRITE */
    int err;    /* first store error, returned by every later call */
    void *handle;
    void *sha;
    size_t len;
    registry_key_t key;
    uint8_t layer;
} stage_t;

static struct {
    platform_mutex_t *lock;
    bool ready;
    registry_store_t *stores[MAX_STORES];
    size_t nStores;
    uint32_t seq;
    uint32_t order;
    stage_t stage;
    pending_t pending[MAX_PENDING];
    map_t maps[MAX_MAPS];
    resident_t resident[MAX_RESIDENT];
    /* Scratch the lock serialises; kept out of the stack on small targets. */
    registry_key_t keys[REGISTRY_MAX_ENTRIES];
    registry_key_t scanKeys[REGISTRY_MAX_ENTRIES];
    uint8_t rec[REGISTRY_RECORD_MAX];
    uint8_t blobs[MAX_BLOBS][REGISTRY_DIGEST_LEN];
} g;

static void lock(void) { PlatformMutexLock(g.lock); }
static void unlock(void) { PlatformMutexUnlock(g.lock); }

static registry_store_t *primary(void) {
    return g.nStores > 0 ? g.stores[0] : NULL;
}

static bool keyEq(const registry_key_t *a, const registry_key_t *b) {
    return strncmp(a->name, b->name, REGISTRY_KEY_NAME_LEN) == 0 &&
           strncmp(a->version, b->version, REGISTRY_KEY_NAME_LEN) == 0;
}

static int keyCmp(const registry_key_t *a, const registry_key_t *b) {
    int c = strncmp(a->name, b->name, REGISTRY_KEY_NAME_LEN);

    return c != 0 ? c : strncmp(a->version, b->version, REGISTRY_KEY_NAME_LEN);
}

static void sortKeys(registry_key_t *keys, size_t n) {
    for (size_t i = 1; i < n; i++) {
        registry_key_t k = keys[i];
        size_t j = i;

        while (j > 0 && keyCmp(&keys[j - 1], &k) > 0) {
            keys[j] = keys[j - 1];
            j--;
        }
        keys[j] = k;
    }
}

/* Splits "<name>[:<version>]" into a key. */
static int parseRef(const char *ref, registry_key_t *key) {
    const char *colon;
    size_t nameLen;
    size_t verLen = 0;

    if (ref == NULL || ref[0] == '\0')
        return -EINVAL;
    colon = strchr(ref, ':');
    nameLen = colon != NULL ? (size_t)(colon - ref) : strlen(ref);
    if (colon != NULL)
        verLen = strlen(colon + 1);
    if (nameLen == 0 || nameLen >= REGISTRY_KEY_NAME_LEN ||
        verLen >= REGISTRY_KEY_NAME_LEN)
        return -ENAMETOOLONG;

    memset(key, 0, sizeof(*key));
    memcpy(key->name, ref, nameLen);
    if (colon != NULL)
        memcpy(key->version, colon + 1, verLen);
    return 0;
}

static void keyFromEntry(const reg_entry_t *e, registry_key_t *key) {
    memset(key, 0, sizeof(*key));
    strncpy(key->name, e->name, REGISTRY_KEY_NAME_LEN - 1);
    strncpy(key->version, e->version, REGISTRY_KEY_NAME_LEN - 1);
}

static resident_t *residentFind(const registry_key_t *key) {
    for (size_t i = 0; i < MAX_RESIDENT; i++) {
        if (g.resident[i].used && keyEq(&g.resident[i].key, key))
            return &g.resident[i];
    }
    return NULL;
}

static resident_t *residentByDigest(const uint8_t *digest) {
    for (size_t i = 0; i < MAX_RESIDENT; i++) {
        if (g.resident[i].used &&
            memcmp(g.resident[i].digest, digest, REGISTRY_DIGEST_LEN) == 0)
            return &g.resident[i];
    }
    return NULL;
}

static void residentMeta(const resident_t *r, registry_meta_t *meta) {
    memset(meta, 0, sizeof(*meta));
    meta->magic = REGISTRY_META_MAGIC;
    meta->layerCount = 1;
    meta->size = (uint32_t)r->len;
    meta->layerLen[0] = (uint32_t)r->len;
    memcpy(meta->layerDigest[0], r->digest, REGISTRY_DIGEST_LEN);
    meta->created = r->created;
    meta->flags = REGISTRY_META_SEEDED | REGISTRY_META_CLOCK_UNSET;
}

/* An installed entry wins over a resident image of the same ref. */
static int readMeta(const registry_key_t *key, registry_meta_t *meta) {
    registry_store_t *p = primary();
    const resident_t *r = residentFind(key);
    int n = -ENODEV;

    if (p != NULL && p->ops->recGet != NULL)
        n = p->ops->recGet(p, key, g.rec, sizeof(g.rec));
    if (n >= 0)
        return RegistryRecordDecode(g.rec, (size_t)n, meta) ? 0 : -ENOENT;
    if (r == NULL)
        return n;
    residentMeta(r, meta);
    return 0;
}

/* The ref has a resident image and no installed entry. */
static bool residentOnly(const registry_key_t *key) {
    registry_store_t *p = primary();

    if (residentFind(key) == NULL)
        return false;
    return p == NULL || p->ops->recGet == NULL ||
           p->ops->recGet(p, key, g.rec, sizeof(g.rec)) < 0;
}

static int writeMeta(const registry_key_t *key, const registry_meta_t *meta) {
    registry_store_t *p = primary();
    size_t n = RegistryRecordEncode(meta, g.rec, sizeof(g.rec));

    if (p == NULL || p->ops->recPut == NULL)
        return -ENODEV;
    if (n == 0)
        return -EINVAL;
    return p->ops->recPut(p, key, g.rec, n);
}

static int listKeys(registry_key_t *keys) {
    registry_store_t *p = primary();
    int n;

    if (p == NULL || p->ops->recList == NULL)
        return -ENODEV;
    n = p->ops->recList(p, keys, REGISTRY_MAX_ENTRIES);
    if (n < 0)
        return n;
    if (n > REGISTRY_MAX_ENTRIES)
        n = REGISTRY_MAX_ENTRIES;
    for (size_t i = 0; i < MAX_RESIDENT && n < REGISTRY_MAX_ENTRIES; i++) {
        bool listed = false;

        if (!g.resident[i].used)
            continue;
        for (int k = 0; k < n && !listed; k++)
            listed = keyEq(&keys[k], &g.resident[i].key);
        if (!listed)
            keys[n++] = g.resident[i].key;
    }
    sortKeys(keys, (size_t)n);
    return n;
}

/* Finds the entry `key` names. A key with no version takes the newest entry of
 * that name, and the first in sort order among entries that tie. */
static int resolve(const registry_key_t *key, registry_key_t *out,
                   registry_meta_t *meta) {
    registry_meta_t cand;
    bool found = false;
    int n;

    if (key->version[0] != '\0') {
        *out = *key;
        return readMeta(out, meta);
    }
    n = listKeys(g.keys);
    if (n < 0)
        return n;
    for (int i = 0; i < n; i++) {
        if (strncmp(g.keys[i].name, key->name, REGISTRY_KEY_NAME_LEN) != 0)
            continue;
        if (readMeta(&g.keys[i], &cand) < 0)
            continue;
        if (!found || cand.installSeq > meta->installSeq) {
            *out = g.keys[i];
            *meta = cand;
            found = true;
        }
    }
    return found ? 0 : -ENOENT;
}

static bool digestEq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, REGISTRY_DIGEST_LEN) == 0;
}

static bool referenced(const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    registry_meta_t meta;
    int n = listKeys(g.scanKeys);

    for (int i = 0; i < n; i++) {
        if (readMeta(&g.scanKeys[i], &meta) < 0)
            continue;
        for (size_t l = 0; l < meta.layerCount; l++) {
            if (digestEq(meta.layerDigest[l], digest))
                return true;
        }
    }
    return false;
}

static bool held(const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        if (g.pending[i].used && digestEq(g.pending[i].digest, digest))
            return true;
    }
    for (size_t i = 0; i < MAX_MAPS; i++) {
        if (g.maps[i].used && digestEq(g.maps[i].digest, digest))
            return true;
    }
    return false;
}

/* Frees a blob nothing lists, stages, or maps. */
static void dropIfFree(const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    if (referenced(digest) || held(digest))
        return;
    for (size_t i = 0; i < g.nStores; i++) {
        registry_store_t *s = g.stores[i];

        if (s->ops->blobHas != NULL && s->ops->blobHas(s, digest))
            s->ops->blobDrop(s, digest);
    }
}

/* Frees every blob no entry lists, after a reset cut an install. */
static void reclaim(void) {
    for (size_t i = 0; i < g.nStores; i++) {
        registry_store_t *s = g.stores[i];
        int n;

        if (s->ops->blobList == NULL)
            continue;
        n = s->ops->blobList(s, g.blobs, MAX_BLOBS);
        if (n > MAX_BLOBS)
            n = MAX_BLOBS;
        for (int b = 0; b < n; b++) {
            if (!referenced(g.blobs[b]))
                s->ops->blobDrop(s, g.blobs[b]);
        }
    }
}

static void ensureReady(void) {
    registry_meta_t meta;
    int n;

    if (g.ready)
        return;
    if (g.lock == NULL)
        g.lock = PlatformMutexNew();
    g.nStores = PlatformRegistryStores(g.stores, MAX_STORES);
    for (size_t i = 0; i < g.nStores; i++) {
        int rc = g.stores[i]->ops->init != NULL
                     ? g.stores[i]->ops->init(g.stores[i])
                     : 0;

        if (rc < 0)
            LOG_ERROR("registry store %zu failed to start: %d", i, rc);
    }
    g.seq = 0;
    n = listKeys(g.keys);
    for (int i = 0; i < n; i++) {
        if (readMeta(&g.keys[i], &meta) == 0 && meta.installSeq > g.seq)
            g.seq = meta.installSeq;
    }
    reclaim();
    g.ready = true;
}

static bool clockValid(int64_t *seconds) {
    plat_timestamp_t ns;

    if (WantedGetClockQuality() >= WANTED_CLOCK_UNCALIBRATED)
        return false;
    if (PlatformClockGetTime(PLAT_CLOCKID_REALTIME, &ns) != 0)
        return false;
    *seconds = (int64_t)(ns / NS_PER_SEC);
    return true;
}

static void pendingDrop(pending_t *p) {
    uint8_t digest[REGISTRY_DIGEST_LEN];

    memcpy(digest, p->digest, sizeof(digest));
    memset(p, 0, sizeof(*p));
    dropIfFree(digest);
}

static void pendingAdd(const registry_key_t *key, uint8_t index,
                       const uint8_t digest[REGISTRY_DIGEST_LEN],
                       uint32_t len) {
    pending_t *slot = NULL;
    pending_t *oldest = NULL;

    for (size_t i = 0; i < MAX_PENDING; i++) {
        pending_t *p = &g.pending[i];

        if (p->used && keyEq(&p->key, key) && p->index == index) {
            slot = p;
            break;
        }
        if (!p->used && slot == NULL)
            slot = p;
        if (p->used && (oldest == NULL || p->order < oldest->order))
            oldest = p;
    }
    if (slot == NULL) {
        pendingDrop(oldest);
        slot = oldest;
    } else if (slot->used) {
        pendingDrop(slot);
    }
    slot->used = true;
    slot->key = *key;
    slot->index = index;
    memcpy(slot->digest, digest, REGISTRY_DIGEST_LEN);
    slot->len = len;
    slot->order = ++g.order;
}

static pending_t *pendingFind(const registry_key_t *key, uint8_t index) {
    for (size_t i = 0; i < MAX_PENDING; i++) {
        pending_t *p = &g.pending[i];

        if (p->used && keyEq(&p->key, key) && p->index == index)
            return p;
    }
    return NULL;
}

/* A store can gain an entry the registry did not commit, so the next sequence
 * follows every record there is. */
static void syncSeq(void) {
    registry_meta_t meta;
    int n = listKeys(g.scanKeys);

    for (int i = 0; i < n; i++) {
        if (readMeta(&g.scanKeys[i], &meta) == 0 && meta.installSeq > g.seq)
            g.seq = meta.installSeq;
    }
}

static int commitLocked(const registry_key_t *key, unsigned count,
                        bool hasCreated, int64_t created) {
    registry_meta_t meta;
    registry_meta_t old;
    uint8_t freed[MAX_PENDING][REGISTRY_DIGEST_LEN];
    size_t nFreed = 0;
    bool hadOld;
    int64_t now;
    int rc;

    if (count == 0)
        return -EINVAL;
    if (count > REGISTRY_META_MAX_LAYERS) {
        LOG_ERROR("%s:%s commit of %u layers exceeds the limit of %u",
                  key->name, key->version, count, REGISTRY_META_MAX_LAYERS);
        return -E2BIG;
    }

    memset(&meta, 0, sizeof(meta));
    meta.magic = REGISTRY_META_MAGIC;
    meta.layerCount = (uint8_t)count;
    for (unsigned i = 0; i < count; i++) {
        const pending_t *p = pendingFind(key, (uint8_t)i);

        if (p == NULL) {
            LOG_ERROR("%s:%s commit of %u layers lacks layer %u", key->name,
                      key->version, count, i);
            return -ENOENT;
        }
        meta.layerLen[i] = p->len;
        memcpy(meta.layerDigest[i], p->digest, REGISTRY_DIGEST_LEN);
        if (meta.size + p->len < meta.size)
            return -EFBIG;
        meta.size += p->len;
    }
    if (clockValid(&now))
        meta.installed = now;
    else
        meta.flags |= REGISTRY_META_CLOCK_UNSET;
    if (hasCreated)
        meta.created = created;
    syncSeq();
    meta.installSeq = g.seq + 1;

    hadOld = readMeta(key, &old) == 0;
    rc = writeMeta(key, &meta);
    if (rc < 0)
        return rc;
    g.seq = meta.installSeq;

    if (hadOld) {
        for (size_t i = 0; i < old.layerCount; i++)
            memcpy(freed[nFreed++], old.layerDigest[i], REGISTRY_DIGEST_LEN);
    }
    for (size_t i = 0; i < MAX_PENDING; i++) {
        pending_t *p = &g.pending[i];

        if (!p->used || !keyEq(&p->key, key))
            continue;
        if (p->index >= count && nFreed < MAX_PENDING)
            memcpy(freed[nFreed++], p->digest, REGISTRY_DIGEST_LEN);
        memset(p, 0, sizeof(*p));
    }
    for (size_t i = 0; i < nFreed; i++)
        dropIfFree(freed[i]);
    return 0;
}

static int stageWriteLocked(const uint8_t *buf, size_t n) {
    registry_store_t *s = primary();
    int w;

    if (g.stage.err != 0)
        return g.stage.err;
    w = s->ops->stageWrite(s, g.stage.handle, buf, n);
    if (w < 0) {
        g.stage.err = w;
        return w;
    }
    if (w > 0) {
        PlatformSha256Update(g.stage.sha, buf, (size_t)w);
        g.stage.len += (size_t)w;
    }
    return w;
}

static void stageClear(void) {
    if (g.stage.sha != NULL)
        PlatformSha256Free(g.stage.sha);
    memset(&g.stage, 0, sizeof(g.stage));
}

static int stageFinishLocked(void) {
    registry_store_t *s = primary();
    uint8_t digest[REGISTRY_DIGEST_LEN];
    registry_key_t key = g.stage.key;
    uint8_t layer = g.stage.layer;
    bool plain = g.stage.plain;
    size_t len = g.stage.len;
    int err = g.stage.err;
    void *handle = g.stage.handle;
    int rc;

    if (err == 0 && len == 0)
        err = -EINVAL;
    if (err != 0) {
        s->ops->stageAbort(s, handle);
        stageClear();
        return err;
    }
    PlatformSha256Final(g.stage.sha, digest);
    stageClear();
    rc = s->ops->stageEnd(s, handle, digest, len);
    if (rc < 0) {
        LOG_ERROR("%s:%s layer %u of %zu bytes refused by the store: %d",
                  key.name, key.version, layer, len, rc);
        return rc;
    }
    pendingAdd(&key, layer, digest, (uint32_t)len);
    if (!plain)
        return 0;
    return commitLocked(&key, 1, false, 0);
}

static int writeLayerLocked(write_state_t st, const char *ref, unsigned layer,
                            bool plain, const uint8_t *buf, size_t n) {
    registry_store_t *s = primary();
    registry_key_t key;
    int rc;

    if (s == NULL || s->ops->stageBegin == NULL)
        return -ENODEV;

    switch (st) {
    case START_WRITE:
        if (buf == NULL || n == 0)
            return -EINVAL;
        if (g.stage.active)
            return -EBUSY;
        rc = parseRef(ref, &key);
        if (rc < 0)
            return rc;
        if (layer >= REGISTRY_META_MAX_LAYERS) {
            LOG_ERROR("%s layer %u is past the limit of %u", ref, layer,
                      REGISTRY_META_MAX_LAYERS);
            return -EINVAL;
        }
        stageClear();
        g.stage.sha = PlatformSha256New();
        if (g.stage.sha == NULL)
            return -ENOMEM;
        rc = s->ops->stageBegin(s, &g.stage.handle);
        if (rc < 0) {
            stageClear();
            return rc;
        }
        g.stage.active = true;
        g.stage.plain = plain;
        g.stage.key = key;
        g.stage.layer = (uint8_t)layer;
        rc = stageWriteLocked(buf, n);
        if (rc < 0) {
            s->ops->stageAbort(s, g.stage.handle);
            stageClear();
        }
        return rc;
    case CONTINUE_WRITE:
        if (!g.stage.active)
            return -EBADF;
        if (buf == NULL || n == 0)
            return -EINVAL;
        return stageWriteLocked(buf, n);
    case FINISH_WRITE:
        if (!g.stage.active)
            return -EBADF;
        return stageFinishLocked();
    case ABORT_WRITE:
        if (!g.stage.active)
            return -EBADF;
        s->ops->stageAbort(s, g.stage.handle);
        stageClear();
        return 0;
    }
    return -EINVAL;
}

int RegistryWriteLayer(write_state_t s, const char *ref, unsigned layer,
                       const uint8_t *buf, size_t nbytes) {
    int rc;

    ensureReady();
    lock();
    rc = writeLayerLocked(s, ref, layer, false, buf, nbytes);
    unlock();
    return rc;
}

int RegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                  size_t nbytes) {
    int rc;

    ensureReady();
    lock();
    rc = writeLayerLocked(s, ref, 0, true, buf, nbytes);
    unlock();
    return rc;
}

int RegistryCommit(const char *ref, unsigned count, bool hasCreated,
                   int64_t created) {
    registry_key_t key;
    int rc = parseRef(ref, &key);

    if (rc < 0)
        return rc;
    ensureReady();
    lock();
    rc = commitLocked(&key, count, hasCreated, created);
    unlock();
    return rc;
}

int RegistryRead(reg_entry_t *list, size_t len) {
    registry_meta_t meta;
    size_t filled = 0;
    int total = 0;
    int n;

    ensureReady();
    lock();
    n = listKeys(g.keys);
    for (int i = 0; n > 0 && i < n; i++) {
        if (readMeta(&g.keys[i], &meta) < 0)
            continue;
        if (list != NULL && filled < len) {
            memset(&list[filled], 0, sizeof(list[filled]));
            strncpy(list[filled].name, g.keys[i].name, WAPP_MAX_NAME_LEN - 1);
            strncpy(list[filled].version, g.keys[i].version,
                    WAPP_MAX_VERSION_LEN - 1);
            list[filled].size = meta.size;
            filled++;
        }
        total++;
    }
    unlock();
    return n < 0 ? n : total;
}

int RegistryRemove(const reg_entry_t *entry) {
    registry_store_t *p;
    registry_key_t want;
    registry_key_t key;
    registry_meta_t meta;
    int rc;

    if (entry == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    p = primary();
    keyFromEntry(entry, &want);
    rc = p == NULL || p->ops->recDel == NULL ? -ENODEV
                                             : resolve(&want, &key, &meta);
    if (rc == 0 && (meta.flags & REGISTRY_META_SEEDED))
        rc = -EPERM;
    if (rc == 0)
        rc = p->ops->recDel(p, &key);
    if (rc == 0) {
        for (size_t i = 0; i < meta.layerCount; i++)
            dropIfFree(meta.layerDigest[i]);
    }
    unlock();
    return rc;
}

static registry_store_t *storeHolding(const uint8_t *digest) {
    for (size_t i = 0; i < g.nStores; i++) {
        registry_store_t *s = g.stores[i];

        if (s->ops->blobHas != NULL && s->ops->blobHas(s, digest))
            return s;
    }
    return NULL;
}

static int mapResident(const resident_t *r, uint8_t **ptr, size_t *outLen) {
    for (size_t i = 0; i < MAX_MAPS; i++) {
        map_t *m = &g.maps[i];

        if (m->used)
            continue;
        m->used = true;
        m->store = NULL;
        m->map.ptr = r->data;
        m->map.len = r->len;
        memcpy(m->digest, r->digest, REGISTRY_DIGEST_LEN);
        *ptr = (uint8_t *)r->data;
        *outLen = r->len;
        return 0;
    }
    return -ENOMEM;
}

static int mapBlob(const uint8_t *digest, size_t len, uint8_t **ptr,
                   size_t *outLen) {
    const resident_t *r = residentByDigest(digest);
    registry_store_t *s;

    if (r != NULL)
        return mapResident(r, ptr, outLen);
    s = storeHolding(digest);
    if (s == NULL)
        return -ENOENT;
    for (size_t i = 0; i < MAX_MAPS; i++) {
        map_t *m = &g.maps[i];
        int rc;

        if (m->used)
            continue;
        rc = s->ops->blobMap(s, digest, len, &m->map);
        if (rc < 0)
            return rc;
        m->used = true;
        m->store = s;
        memcpy(m->digest, digest, REGISTRY_DIGEST_LEN);
        *ptr = (uint8_t *)m->map.ptr;
        *outLen = m->map.len;
        return 0;
    }
    return -ENOMEM;
}

static void unmapPtr(const uint8_t *ptr) {
    for (size_t i = 0; i < MAX_MAPS; i++) {
        map_t *m = &g.maps[i];
        uint8_t digest[REGISTRY_DIGEST_LEN];

        if (!m->used || m->map.ptr != ptr)
            continue;
        memcpy(digest, m->digest, sizeof(digest));
        if (m->store != NULL)
            m->store->ops->blobUnmap(m->store, &m->map);
        memset(m, 0, sizeof(*m));
        dropIfFree(digest);
        return;
    }
}

static int mapLayersLocked(const reg_entry_t *entry, uint8_t **layers,
                           size_t *lens, uint8_t *count,
                           registry_key_t *resolved) {
    registry_key_t want;
    registry_meta_t meta;
    int rc;

    keyFromEntry(entry, &want);
    rc = resolve(&want, resolved, &meta);
    if (rc < 0)
        return rc;
    for (size_t i = 0; i < meta.layerCount; i++) {
        size_t base = meta.layerCount - 1 - i;

        rc = mapBlob(meta.layerDigest[base], meta.layerLen[base], &layers[i],
                     &lens[i]);
        if (rc < 0) {
            for (size_t j = 0; j < i; j++)
                unmapPtr(layers[j]);
            return rc;
        }
    }
    *count = meta.layerCount;
    return 0;
}

int RegistryMapLayers(const reg_entry_t *entry,
                      uint8_t *layers[REGISTRY_META_MAX_LAYERS],
                      size_t lens[REGISTRY_META_MAX_LAYERS], uint8_t *count) {
    registry_key_t resolved;
    int rc;

    if (entry == NULL || layers == NULL || lens == NULL || count == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    rc = mapLayersLocked(entry, layers, lens, count, &resolved);
    unlock();
    return rc;
}

void RegistryUnmapLayers(uint8_t *const layers[], uint8_t count) {
    lock();
    for (uint8_t i = 0; i < count; i++)
        unmapPtr(layers[i]);
    unlock();
}

int RegistryLoad(const reg_entry_t *entry, wapp_t *w) {
    uint8_t *layers[REGISTRY_META_MAX_LAYERS];
    size_t lens[REGISTRY_META_MAX_LAYERS];
    registry_key_t resolved;
    uint8_t count = 0;
    int rc;

    if (entry == NULL || w == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    rc = mapLayersLocked(entry, layers, lens, &count, &resolved);
    unlock();
    if (rc < 0)
        return rc;

    for (uint8_t i = 0; i < count; i++) {
        w->layers[i] = layers[i];
        w->layer_lens[i] = lens[i];
    }
    w->layer_cnt = count;
    strncpy(w->image, resolved.name, WAPP_MAX_NAME_LEN - 1);
    w->image[WAPP_MAX_NAME_LEN - 1] = '\0';
    strncpy(w->version, resolved.version, WAPP_MAX_VERSION_LEN - 1);
    w->version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    return 0;
}

bool RegistryUnload(const wapp_t *w) {
    bool any = false;

    if (w == NULL || !g.ready)
        return false;
    lock();
    for (uint8_t i = 0; i < w->layer_cnt && i < REGISTRY_META_MAX_LAYERS; i++) {
        for (size_t m = 0; m < MAX_MAPS; m++) {
            if (g.maps[m].used && g.maps[m].map.ptr == w->layers[i]) {
                unmapPtr(w->layers[i]);
                any = true;
                break;
            }
        }
    }
    unlock();
    return any;
}

int RegistryMetaRead(const reg_entry_t *entry, registry_meta_t *out) {
    registry_key_t want;
    registry_key_t key;
    int rc;

    if (entry == NULL || out == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    keyFromEntry(entry, &want);
    rc = resolve(&want, &key, out);
    unlock();
    return rc;
}

int RegistryMetaSetSignature(const reg_entry_t *entry, uint32_t keyId,
                             const uint8_t sig[REGISTRY_META_SIG_LEN]) {
    registry_key_t want;
    registry_key_t key;
    registry_meta_t meta;
    int rc;

    if (entry == NULL || sig == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    keyFromEntry(entry, &want);
    rc = resolve(&want, &key, &meta);
    if (rc == 0 && residentOnly(&key))
        rc = -EPERM;
    if (rc == 0) {
        meta.keyId = keyId;
        memcpy(meta.signature, sig, REGISTRY_META_SIG_LEN);
        meta.flags |= REGISTRY_META_SIGNED;
        rc = writeMeta(&key, &meta);
    }
    unlock();
    return rc;
}

bool RegistryIsMapped(const reg_entry_t *entry) {
    registry_key_t want;
    registry_key_t key;
    registry_meta_t meta;
    bool mapped = false;

    if (entry == NULL)
        return false;
    ensureReady();
    lock();
    keyFromEntry(entry, &want);
    if (resolve(&want, &key, &meta) == 0) {
        for (size_t i = 0; i < meta.layerCount && !mapped; i++) {
            for (size_t m = 0; m < MAX_MAPS; m++) {
                if (g.maps[m].used &&
                    digestEq(g.maps[m].digest, meta.layerDigest[i]))
                    mapped = true;
            }
        }
    }
    unlock();
    return mapped;
}

void RegistryMarkSeeded(const char *ref) {
    registry_key_t want;
    registry_key_t key;
    registry_meta_t meta;

    if (parseRef(ref, &want) < 0)
        return;
    ensureReady();
    lock();
    if (resolve(&want, &key, &meta) == 0 && !residentOnly(&key) &&
        !(meta.flags & REGISTRY_META_SEEDED)) {
        meta.flags |= REGISTRY_META_SEEDED;
        writeMeta(&key, &meta);
    }
    unlock();
}

int RegistryAddResident(const char *ref, const uint8_t *data, size_t len,
                        int64_t created) {
    registry_key_t key;
    resident_t *slot = NULL;
    void *sha;
    int rc;

    if (data == NULL || len == 0)
        return -EINVAL;
    rc = parseRef(ref, &key);
    if (rc < 0)
        return rc;
    sha = PlatformSha256New();
    if (sha == NULL)
        return -ENOMEM;

    slot = residentFind(&key);
    for (size_t i = 0; i < MAX_RESIDENT && slot == NULL; i++) {
        if (!g.resident[i].used)
            slot = &g.resident[i];
    }
    if (slot == NULL) {
        PlatformSha256Free(sha);
        return -ENOSPC;
    }
    PlatformSha256Update(sha, data, len);
    PlatformSha256Final(sha, slot->digest);
    PlatformSha256Free(sha);
    slot->used = true;
    slot->key = key;
    slot->data = data;
    slot->len = len;
    slot->created = created;
    return 0;
}

int RegistryCapacity(registry_capacity_t *out, size_t max) {
    size_t n = 0;

    ensureReady();
    lock();
    for (size_t i = 0; i < g.nStores && out != NULL && n < max; i++) {
        registry_store_t *s = g.stores[i];

        if (s->ops->capacity == NULL)
            continue;
        memset(&out[n], 0, sizeof(out[n]));
        if (s->ops->capacity(s, &out[n]) == 0)
            n++;
    }
    unlock();
    return (int)n;
}

static bool blobPresent(const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    return residentByDigest(digest) != NULL || storeHolding(digest) != NULL;
}

static int blobIndex(const uint8_t digest[REGISTRY_DIGEST_LEN], int count) {
    for (int i = 0; i < count; i++) {
        if (digestEq(g.blobs[i], digest))
            return i;
    }
    return -1;
}

/* Fills g.blobs with each distinct digest a listed entry names and something
 * holds, and returns how many. */
static int visibleBlobs(void) {
    registry_meta_t meta;
    int n = listKeys(g.keys);
    int count = 0;

    for (int i = 0; i < n; i++) {
        if (readMeta(&g.keys[i], &meta) < 0)
            continue;
        for (size_t l = 0; l < meta.layerCount; l++) {
            const uint8_t *d = meta.layerDigest[l];

            if (blobIndex(d, count) >= 0 || !blobPresent(d))
                continue;
            if (count == MAX_BLOBS)
                return count;
            memcpy(g.blobs[count++], d, REGISTRY_DIGEST_LEN);
        }
    }
    return count;
}

/* The length and the entry count of a blob that is listed and held. */
static bool blobLookup(const uint8_t digest[REGISTRY_DIGEST_LEN], uint32_t *len,
                       unsigned *entries) {
    registry_meta_t meta;
    int n = listKeys(g.keys);

    *entries = 0;
    for (int i = 0; i < n; i++) {
        if (readMeta(&g.keys[i], &meta) < 0)
            continue;
        for (size_t l = 0; l < meta.layerCount; l++) {
            if (!digestEq(meta.layerDigest[l], digest))
                continue;
            *len = meta.layerLen[l];
            (*entries)++;
            break;
        }
    }
    return *entries > 0 && blobPresent(digest);
}

int RegistryBlobAt(size_t n, uint8_t digest[REGISTRY_DIGEST_LEN]) {
    int count;
    int rc = 0;

    ensureReady();
    lock();
    count = visibleBlobs();
    if (n >= (size_t)count)
        rc = -ENOENT;
    else
        memcpy(digest, g.blobs[n], REGISTRY_DIGEST_LEN);
    unlock();
    return rc;
}

int RegistryBlobInfo(const uint8_t digest[REGISTRY_DIGEST_LEN],
                     registry_blob_info_t *out) {
    registry_store_t *s;
    registry_capacity_t cap;
    uint32_t len = 0;
    unsigned entries;
    int rc = 0;

    if (digest == NULL || out == NULL)
        return -EINVAL;
    ensureReady();
    lock();
    if (!blobLookup(digest, &len, &entries)) {
        unlock();
        return -ENOENT;
    }
    memset(out, 0, sizeof(*out));
    out->size = len;
    out->entries = (uint16_t)entries;
    out->resident = residentByDigest(digest) != NULL;
    for (size_t m = 0; m < MAX_MAPS; m++) {
        if (g.maps[m].used && digestEq(g.maps[m].digest, digest))
            out->mapped = true;
    }
    s = out->resident ? NULL : storeHolding(digest);
    if (s != NULL && s->ops->capacity != NULL && s->ops->capacity(s, &cap) == 0)
        strncpy(out->store, cap.name, sizeof(out->store) - 1);
    else if (out->resident)
        strncpy(out->store, "resident", sizeof(out->store) - 1);
    unlock();
    return rc;
}

int RegistryLinkLayer(const char *ref, unsigned layer,
                      const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    registry_key_t key;
    uint32_t len = 0;
    unsigned entries;
    int rc = parseRef(ref, &key);

    if (rc < 0)
        return rc;
    if (layer >= REGISTRY_META_MAX_LAYERS) {
        LOG_ERROR("%s layer %u is past the limit of %u", ref, layer,
                  REGISTRY_META_MAX_LAYERS);
        return -EINVAL;
    }
    ensureReady();
    lock();
    if (!blobLookup(digest, &len, &entries))
        rc = -ENOENT;
    else
        pendingAdd(&key, (uint8_t)layer, digest, len);
    unlock();
    return rc;
}

void RegistryClearResident(void) { memset(g.resident, 0, sizeof(g.resident)); }

void RegistryDeinit(void) {
    if (!g.ready)
        return;
    lock();
    if (g.stage.active)
        primary()->ops->stageAbort(primary(), g.stage.handle);
    stageClear();
    for (size_t i = 0; i < MAX_MAPS; i++) {
        if (g.maps[i].used && g.maps[i].store != NULL)
            g.maps[i].store->ops->blobUnmap(g.maps[i].store, &g.maps[i].map);
    }
    memset(g.maps, 0, sizeof(g.maps));
    memset(g.pending, 0, sizeof(g.pending));
    g.ready = false;
    unlock();
}
