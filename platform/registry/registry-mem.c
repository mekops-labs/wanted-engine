/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <registry-mem.h>
#include <wanted_malloc.h>

#define MAX_BLOBS 64
/* Past the registry limit, so tests can hold more records than it lists. */
#define MAX_RECS 64
#define STAGE_MIN 1024

typedef struct {
    bool used;
    uint8_t digest[REGISTRY_DIGEST_LEN];
    uint8_t *data;
    size_t len;
    unsigned maps;
} blob_t;

typedef struct {
    bool used;
    registry_key_t key;
    uint8_t *data;
    size_t len;
} rec_t;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} stage_t;

static struct {
    blob_t blobs[MAX_BLOBS];
    rec_t recs[MAX_RECS];
    size_t used;
    size_t limit;
    bool failList;
} g;

static registry_store_t store;

static bool digestEq(const uint8_t *a, const uint8_t *b) {
    return memcmp(a, b, REGISTRY_DIGEST_LEN) == 0;
}

static blob_t *findBlob(const uint8_t *digest) {
    for (size_t i = 0; i < MAX_BLOBS; i++) {
        if (g.blobs[i].used && digestEq(g.blobs[i].digest, digest))
            return &g.blobs[i];
    }
    return NULL;
}

static int _StageBegin(registry_store_t *s, void **stage) {
    stage_t *st = WantedMalloc(sizeof(*st));

    (void)s;
    if (st == NULL)
        return -ENOMEM;
    memset(st, 0, sizeof(*st));
    *stage = st;
    return 0;
}

static int _StageWrite(registry_store_t *s, void *stage, const uint8_t *buf,
                       size_t n) {
    stage_t *st = stage;

    (void)s;
    if (g.limit != 0 && g.used + st->len + n > g.limit)
        return -ENOSPC;
    if (st->len + n > st->cap) {
        size_t cap = st->cap == 0 ? STAGE_MIN : st->cap;
        uint8_t *grown;

        while (cap < st->len + n)
            cap *= 2;
        grown = WantedMalloc(cap);
        if (grown == NULL)
            return -ENOMEM;
        if (st->len > 0)
            memcpy(grown, st->data, st->len);
        WantedFree(st->data);
        st->data = grown;
        st->cap = cap;
    }
    memcpy(st->data + st->len, buf, n);
    st->len += n;
    return (int)n;
}

static void stageFree(stage_t *st) {
    WantedFree(st->data);
    WantedFree(st);
}

static int _StageEnd(registry_store_t *s, void *stage,
                     const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len) {
    stage_t *st = stage;
    blob_t *b;

    (void)s;
    (void)len;
    if (findBlob(digest) != NULL) {
        stageFree(st);
        return 0;
    }
    for (size_t i = 0; i < MAX_BLOBS; i++) {
        if (g.blobs[i].used)
            continue;
        b = &g.blobs[i];
        b->used = true;
        memcpy(b->digest, digest, REGISTRY_DIGEST_LEN);
        b->data = st->data;
        b->len = st->len;
        b->maps = 0;
        g.used += st->len;
        WantedFree(st);
        return 0;
    }
    stageFree(st);
    return -ENOSPC;
}

static void _StageAbort(registry_store_t *s, void *stage) {
    (void)s;
    stageFree(stage);
}

static int _BlobMap(registry_store_t *s,
                    const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len,
                    registry_blob_map_t *out) {
    blob_t *b = findBlob(digest);

    (void)s;
    (void)len;
    if (b == NULL)
        return -ENOENT;
    b->maps++;
    out->ptr = b->data;
    out->len = b->len;
    out->handle = b;
    return 0;
}

static void _BlobUnmap(registry_store_t *s, registry_blob_map_t *map) {
    blob_t *b = map->handle;

    (void)s;
    if (b != NULL && b->maps > 0)
        b->maps--;
}

static bool _BlobHas(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    (void)s;
    return findBlob(digest) != NULL;
}

static int _BlobDrop(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    blob_t *b = findBlob(digest);

    (void)s;
    if (b == NULL)
        return -ENOENT;
    if (b->maps > 0)
        return -EBUSY;
    g.used -= b->len;
    WantedFree(b->data);
    memset(b, 0, sizeof(*b));
    return 0;
}

static int _BlobList(registry_store_t *s,
                     uint8_t (*digests)[REGISTRY_DIGEST_LEN], size_t max) {
    int n = 0;

    (void)s;
    for (size_t i = 0; i < MAX_BLOBS; i++) {
        if (!g.blobs[i].used)
            continue;
        if ((size_t)n < max)
            memcpy(digests[n], g.blobs[i].digest, REGISTRY_DIGEST_LEN);
        n++;
    }
    return n;
}

static rec_t *findRec(const registry_key_t *key) {
    for (size_t i = 0; i < MAX_RECS; i++) {
        if (g.recs[i].used &&
            strncmp(g.recs[i].key.name, key->name, REGISTRY_KEY_NAME_LEN) ==
                0 &&
            strncmp(g.recs[i].key.version, key->version,
                    REGISTRY_KEY_NAME_LEN) == 0)
            return &g.recs[i];
    }
    return NULL;
}

static int _RecPut(registry_store_t *s, const registry_key_t *key,
                   const uint8_t *rec, size_t len) {
    rec_t *r = findRec(key);
    uint8_t *copy;

    (void)s;
    if (r == NULL) {
        for (size_t i = 0; i < MAX_RECS && r == NULL; i++) {
            if (!g.recs[i].used)
                r = &g.recs[i];
        }
    }
    if (r == NULL)
        return -ENOSPC;
    copy = WantedMalloc(len);
    if (copy == NULL)
        return -ENOMEM;
    memcpy(copy, rec, len);
    WantedFree(r->data);
    r->used = true;
    r->key = *key;
    r->data = copy;
    r->len = len;
    return 0;
}

static int _RecGet(registry_store_t *s, const registry_key_t *key, uint8_t *rec,
                   size_t cap) {
    const rec_t *r = findRec(key);

    (void)s;
    if (r == NULL)
        return -ENOENT;
    if (r->len > cap)
        return -EMSGSIZE;
    memcpy(rec, r->data, r->len);
    return (int)r->len;
}

static int _RecDel(registry_store_t *s, const registry_key_t *key) {
    rec_t *r = findRec(key);

    (void)s;
    if (r == NULL)
        return -ENOENT;
    WantedFree(r->data);
    memset(r, 0, sizeof(*r));
    return 0;
}

static int _RecList(registry_store_t *s, registry_key_t *keys, size_t max) {
    int n = 0;

    (void)s;
    if (g.failList)
        return -EIO;
    for (size_t i = 0; i < MAX_RECS; i++) {
        if (!g.recs[i].used)
            continue;
        if ((size_t)n < max)
            keys[n] = g.recs[i].key;
        n++;
    }
    return n;
}

static int _Capacity(registry_store_t *s, registry_capacity_t *out) {
    (void)s;
    strncpy(out->name, "mem", sizeof(out->name) - 1);
    strncpy(out->kind, "mem", sizeof(out->kind) - 1);
    out->total = (uint32_t)g.limit;
    out->freeBytes = g.limit != 0 ? (uint32_t)(g.limit - g.used) : 0;
    out->largestFree = out->freeBytes;
    out->caps = REGISTRY_CAP_MAPPABLE | REGISTRY_CAP_SCATTERED;
    out->present = true;
    return 0;
}

static const registry_store_ops_t ops = {
    .stageBegin = _StageBegin,
    .stageWrite = _StageWrite,
    .stageEnd = _StageEnd,
    .stageAbort = _StageAbort,
    .blobMap = _BlobMap,
    .blobUnmap = _BlobUnmap,
    .blobHas = _BlobHas,
    .blobDrop = _BlobDrop,
    .blobList = _BlobList,
    .recPut = _RecPut,
    .recGet = _RecGet,
    .recDel = _RecDel,
    .recList = _RecList,
    .capacity = _Capacity,
};

registry_store_t *RegistryMemStore(void) {
    store.ops = &ops;
    return &store;
}

void RegistryMemReset(void) {
    for (size_t i = 0; i < MAX_BLOBS; i++)
        WantedFree(g.blobs[i].data);
    for (size_t i = 0; i < MAX_RECS; i++)
        WantedFree(g.recs[i].data);
    memset(&g, 0, sizeof(g));
}

void RegistryMemSetLimit(size_t bytes) { g.limit = bytes; }

void RegistryMemFailList(bool fail) { g.failList = fail; }
