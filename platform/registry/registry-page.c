/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <debug_trace.h>
#include <registry-files.h>
#include <registry-page.h>
#include <wanted_malloc.h>

#define BLOB_MAGIC 0x314c4257U /* "WBL1" */
#define BLOB_HEADER_LEN 44
#define BLOB_EXTENT_LEN 8
#define BLOB_REC_MAX                                                           \
    (BLOB_HEADER_LEN + (REGISTRY_PAGE_MAX_EXTENTS * BLOB_EXTENT_LEN))
#define HEX_LEN (2 * REGISTRY_DIGEST_LEN)
#define STAGE_NAME "_stage"
#define DIR_PATH_MAX 128

typedef struct {
    uint32_t first;
    uint32_t count;
} run_t;

static struct {
    const registry_page_config_t *cfg;
    registry_store_t store;
    uint8_t refs[REGISTRY_PAGE_MAX_UNITS]; /* blobs and stages on a unit */
    uint8_t
        adopted[REGISTRY_PAGE_MAX_UNITS / 8]; /* units an adopted blob names */
    struct {
        bool active;
        run_t runs[REGISTRY_PAGE_MAX_EXTENTS];
        size_t nRuns;
        uint32_t curOff; /* bytes written in the last unit taken */
        uint64_t total;
    } stage;
} g;

/* A blob placed by a layout whose extents need not align to units. */
#define BLOB_FLAG_ADOPTED 0x01

typedef struct {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    uint32_t length;
    uint8_t flags;
    registry_extent_t ext[REGISTRY_PAGE_MAX_EXTENTS];
    size_t nExt;
} blob_rec_t;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Layout: magic u32, extent count u8, flags u8, two reserved bytes, length u32,
 * digest, then each extent as an offset u32 and a length u32. */
static size_t encodeBlob(const blob_rec_t *b, uint8_t *out) {
    size_t off = BLOB_HEADER_LEN;

    memset(out, 0, BLOB_REC_MAX);
    put32(out, BLOB_MAGIC);
    out[4] = (uint8_t)b->nExt;
    out[5] = b->flags;
    put32(out + 8, b->length);
    memcpy(out + 12, b->digest, REGISTRY_DIGEST_LEN);
    for (size_t i = 0; i < b->nExt; i++) {
        put32(out + off, b->ext[i].offset);
        put32(out + off + 4, b->ext[i].length);
        off += BLOB_EXTENT_LEN;
    }
    return off;
}

static bool decodeBlob(const uint8_t *in, size_t len, blob_rec_t *b) {
    size_t n;

    if (len < BLOB_HEADER_LEN || get32(in) != BLOB_MAGIC)
        return false;
    n = in[4];
    if (n == 0 || n > REGISTRY_PAGE_MAX_EXTENTS ||
        len < BLOB_HEADER_LEN + (n * BLOB_EXTENT_LEN))
        return false;
    b->nExt = n;
    b->flags = in[5];
    b->length = get32(in + 8);
    memcpy(b->digest, in + 12, REGISTRY_DIGEST_LEN);
    for (size_t i = 0; i < n; i++) {
        b->ext[i].offset = get32(in + BLOB_HEADER_LEN + (i * BLOB_EXTENT_LEN));
        b->ext[i].length =
            get32(in + BLOB_HEADER_LEN + (i * BLOB_EXTENT_LEN) + 4);
    }
    return true;
}

static void toHex(const uint8_t *digest, char *out) {
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0; i < REGISTRY_DIGEST_LEN; i++) {
        out[2 * i] = digits[digest[i] >> 4];
        out[(2 * i) + 1] = digits[digest[i] & 0x0f];
    }
    out[HEX_LEN] = '\0';
}

static int hexVal(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}

static bool fromHex(const char *s, uint8_t *digest) {
    if (strlen(s) != HEX_LEN)
        return false;
    for (size_t i = 0; i < REGISTRY_DIGEST_LEN; i++) {
        int hi = hexVal(s[2 * i]);
        int lo = hexVal(s[(2 * i) + 1]);

        if (hi < 0 || lo < 0)
            return false;
        digest[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool unitUsed(uint32_t u) { return g.refs[u] > 0; }

static void setUnit(uint32_t u, bool on) {
    if (on && g.refs[u] < UINT8_MAX)
        g.refs[u]++;
    else if (!on && g.refs[u] > 0)
        g.refs[u]--;
}

static bool unitAdopted(uint32_t u) {
    return (g.adopted[u / 8] & (1U << (u % 8))) != 0;
}

/* Takes or gives back the units a byte range touches. A unit an adopted blob
 * named stays marked while any blob uses it. */
static void markRange(uint32_t offset, uint32_t length, bool on, bool adopted) {
    uint32_t unit = g.cfg->unit;
    uint32_t first = offset / unit;
    uint32_t last = (offset + length - 1) / unit;

    for (uint32_t u = first; u <= last && u < g.cfg->units; u++) {
        setUnit(u, on);
        if (adopted && on)
            g.adopted[u / 8] |= (uint8_t)(1U << (u % 8));
        if (!unitUsed(u))
            g.adopted[u / 8] &= (uint8_t)~(1U << (u % 8));
    }
}

static void freeStageUnits(void) {
    for (size_t i = 0; i < g.stage.nRuns; i++) {
        for (uint32_t u = 0; u < g.stage.runs[i].count; u++)
            setUnit(g.stage.runs[i].first + u, false);
    }
    memset(&g.stage, 0, sizeof(g.stage));
}

static uint32_t countFree(void) {
    uint32_t n = 0;

    for (uint32_t u = 0; u < g.cfg->units; u++) {
        if (!unitUsed(u))
            n++;
    }
    return n;
}

/* The start and length, in units, of the longest free run. */
static uint32_t largestRun(uint32_t *start) {
    uint32_t best = 0;
    uint32_t bestStart = 0;
    uint32_t run = 0;

    for (uint32_t u = 0; u < g.cfg->units; u++) {
        if (unitUsed(u)) {
            run = 0;
            continue;
        }
        run++;
        if (run > best) {
            best = run;
            bestStart = u + 1 - run;
        }
    }
    *start = bestStart;
    return best;
}

static bool firstFree(uint32_t *unit) {
    for (uint32_t u = 0; u < g.cfg->units; u++) {
        if (!unitUsed(u)) {
            *unit = u;
            return true;
        }
    }
    return false;
}

/* Takes the next unit for the stage: the one after the last taken when it is
 * free, else a unit elsewhere on a store that can join extents. */
static int takeUnit(uint32_t *unit) {
    run_t *last = g.stage.nRuns > 0 ? &g.stage.runs[g.stage.nRuns - 1] : NULL;
    uint32_t u = 0;
    bool extend = false;

    if (last != NULL) {
        u = last->first + last->count;
        extend = u < g.cfg->units && !unitUsed(u);
    }
    if (!extend) {
        if (last != NULL && !g.cfg->scattered)
            return -ENOSPC;
        if (g.stage.nRuns >= REGISTRY_PAGE_MAX_EXTENTS)
            return -ENOSPC;
        if (g.cfg->scattered) {
            if (!firstFree(&u))
                return -ENOSPC;
        } else if (largestRun(&u) == 0) {
            return -ENOSPC;
        }
        g.stage.runs[g.stage.nRuns].first = u;
        g.stage.runs[g.stage.nRuns].count = 0;
        g.stage.nRuns++;
        last = &g.stage.runs[g.stage.nRuns - 1];
    }
    setUnit(u, true);
    last->count++;
    *unit = u;
    return 0;
}

static int _StageBegin(registry_store_t *s, void **stage) {
    (void)s;
    if (g.stage.active)
        return -EBUSY;
    memset(&g.stage, 0, sizeof(g.stage));
    g.stage.active = true;
    g.stage.curOff = g.cfg->unit; /* the first write takes a unit */
    *stage = &g.stage;
    return 0;
}

static int _StageWrite(registry_store_t *s, void *stage, const uint8_t *buf,
                       size_t n) {
    size_t done = 0;

    (void)s;
    (void)stage;
    while (done < n) {
        const run_t *last;
        uint32_t room;
        size_t chunk;
        int rc;

        if (g.stage.curOff >= g.cfg->unit) {
            uint32_t u;

            rc = takeUnit(&u);
            if (rc < 0)
                return rc;
            rc = g.cfg->flash->erase(g.cfg->flashCtx, u);
            if (rc < 0)
                return rc;
            g.stage.curOff = 0;
        }
        last = &g.stage.runs[g.stage.nRuns - 1];
        room = g.cfg->unit - g.stage.curOff;
        chunk = n - done < room ? n - done : room;
        rc = g.cfg->flash->write(
            g.cfg->flashCtx,
            ((last->first + last->count - 1) * g.cfg->unit) + g.stage.curOff,
            buf + done, chunk);
        if (rc < 0)
            return rc;
        g.stage.curOff += (uint32_t)chunk;
        g.stage.total += chunk;
        done += chunk;
    }
    return (int)n;
}

static int barrier(void) {
    return g.cfg->flash->barrier != NULL
               ? g.cfg->flash->barrier(g.cfg->flashCtx)
               : 0;
}

/* The runs the stage took as byte extents; the last one ends with the data. */
static void stageExtents(blob_rec_t *b) {
    uint64_t left = g.stage.total;

    b->nExt = g.stage.nRuns;
    for (size_t i = 0; i < g.stage.nRuns; i++) {
        uint64_t span = (uint64_t)g.stage.runs[i].count * g.cfg->unit;

        b->ext[i].offset = g.stage.runs[i].first * g.cfg->unit;
        b->ext[i].length = (uint32_t)(span < left ? span : left);
        left -= b->ext[i].length;
    }
}

static int _StageEnd(registry_store_t *s, void *stage,
                     const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len) {
    char name[HEX_LEN + 1];
    uint8_t rec[BLOB_REC_MAX];
    blob_rec_t b;
    size_t n;
    int rc;

    (void)s;
    (void)stage;
    toHex(digest, name);
    if (RegFilesGet(g.cfg->blobDir, name, rec, sizeof(rec)) > 0) {
        freeStageUnits();
        return 0;
    }
    if (len == 0 || g.stage.total != len) {
        freeStageUnits();
        return -EINVAL;
    }
    memset(&b, 0, sizeof(b));
    memcpy(b.digest, digest, REGISTRY_DIGEST_LEN);
    b.length = (uint32_t)len;
    stageExtents(&b);
    n = encodeBlob(&b, rec);

    rc = barrier();
    if (rc == 0)
        rc = RegFilesPut(g.cfg->blobDir, name, rec, n);
    if (rc < 0) {
        freeStageUnits();
        return rc;
    }
    memset(&g.stage, 0, sizeof(g.stage));
    return 0;
}

static void _StageAbort(registry_store_t *s, void *stage) {
    (void)s;
    (void)stage;
    freeStageUnits();
}

static int loadBlob(const uint8_t digest[REGISTRY_DIGEST_LEN], blob_rec_t *b) {
    char name[HEX_LEN + 1];
    uint8_t rec[BLOB_REC_MAX];
    int n;

    toHex(digest, name);
    n = RegFilesGet(g.cfg->blobDir, name, rec, sizeof(rec));
    if (n < 0)
        return n;
    return decodeBlob(rec, (size_t)n, b) ? 0 : -ENOENT;
}

static int _BlobMap(registry_store_t *s,
                    const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len,
                    registry_blob_map_t *out) {
    blob_rec_t b;
    const uint8_t *ptr = NULL;
    void *handle = NULL;
    int rc;

    (void)s;
    (void)len;
    rc = loadBlob(digest, &b);
    if (rc < 0)
        return rc;
    rc =
        g.cfg->flash->map(g.cfg->flashCtx, b.ext, g.cfg->scattered ? b.nExt : 1,
                          b.length, &ptr, &handle);
    if (rc < 0)
        return rc;
    out->ptr = ptr;
    out->len = b.length;
    out->handle = handle;
    return 0;
}

static void _BlobUnmap(registry_store_t *s, registry_blob_map_t *map) {
    (void)s;
    g.cfg->flash->unmap(g.cfg->flashCtx, map->handle);
}

static bool _BlobHas(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    blob_rec_t b;

    (void)s;
    return loadBlob(digest, &b) == 0;
}

static int _BlobDrop(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    char name[HEX_LEN + 1];
    blob_rec_t b;
    int rc = loadBlob(digest, &b);

    (void)s;
    if (rc < 0)
        return rc;
    toHex(digest, name);
    rc = RegFilesDel(g.cfg->blobDir, name);
    if (rc < 0)
        return rc;
    for (size_t i = 0; i < b.nExt; i++)
        markRange(b.ext[i].offset, b.ext[i].length, false,
                  (b.flags & BLOB_FLAG_ADOPTED) != 0);
    return 0;
}

typedef struct {
    uint8_t (*digests)[REGISTRY_DIGEST_LEN];
    size_t max;
    int n;
} list_ctx_t;

static int visitBlob(const char *name, void *arg) {
    list_ctx_t *c = arg;
    uint8_t digest[REGISTRY_DIGEST_LEN];

    if (!fromHex(name, digest))
        return 0;
    if ((size_t)c->n < c->max)
        memcpy(c->digests[c->n], digest, REGISTRY_DIGEST_LEN);
    c->n++;
    return 0;
}

static int _BlobList(registry_store_t *s,
                     uint8_t (*digests)[REGISTRY_DIGEST_LEN], size_t max) {
    list_ctx_t c = {digests, max, 0};
    int rc;

    (void)s;
    rc = RegFilesList(g.cfg->blobDir, visitBlob, &c);
    return rc < 0 ? rc : c.n;
}

static int _RecPut(registry_store_t *s, const registry_key_t *key,
                   const uint8_t *rec, size_t len) {
    char name[REGISTRY_KEY_NAME_LEN * 2 + 8];
    int rc = RegFilesKeyName(name, sizeof(name), key);

    (void)s;
    if (rc < 0)
        return rc;
    rc = barrier();
    return rc < 0 ? rc : RegFilesPut(g.cfg->entryDir, name, rec, len);
}

static int _RecGet(registry_store_t *s, const registry_key_t *key, uint8_t *rec,
                   size_t cap) {
    char name[REGISTRY_KEY_NAME_LEN * 2 + 8];
    int rc = RegFilesKeyName(name, sizeof(name), key);

    (void)s;
    return rc < 0 ? rc : RegFilesGet(g.cfg->entryDir, name, rec, cap);
}

static int _RecDel(registry_store_t *s, const registry_key_t *key) {
    char name[REGISTRY_KEY_NAME_LEN * 2 + 8];
    int rc = RegFilesKeyName(name, sizeof(name), key);

    (void)s;
    if (rc < 0)
        return rc;
    rc = barrier();
    return rc < 0 ? rc : RegFilesDel(g.cfg->entryDir, name);
}

typedef struct {
    registry_key_t *keys;
    size_t max;
    int n;
} key_ctx_t;

static int visitKey(const char *name, void *arg) {
    key_ctx_t *c = arg;
    registry_key_t key;

    if (!RegFilesParseKey(name, &key))
        return 0;
    if ((size_t)c->n < c->max)
        c->keys[c->n] = key;
    c->n++;
    return 0;
}

static int _RecList(registry_store_t *s, registry_key_t *keys, size_t max) {
    key_ctx_t c = {keys, max, 0};
    int rc;

    (void)s;
    rc = RegFilesList(g.cfg->entryDir, visitKey, &c);
    return rc < 0 ? rc : c.n;
}

static int _Capacity(registry_store_t *s, registry_capacity_t *out) {
    uint32_t start;
    uint32_t unit = g.cfg->unit;

    (void)s;
    strncpy(out->name, g.cfg->name, sizeof(out->name) - 1);
    strncpy(out->kind, "page", sizeof(out->kind) - 1);
    out->unit = unit;
    out->total = g.cfg->units * unit;
    out->freeBytes = countFree() * unit;
    out->largestFree = largestRun(&start) * unit;
    out->caps =
        REGISTRY_CAP_MAPPABLE | (g.cfg->scattered ? REGISTRY_CAP_SCATTERED : 0);
    out->present = true;
    return 0;
}

/* Whether the units a blob names are inside the partition and, but for a unit
 * the blob shares with a neighbour by design, free of another blob. */
static bool claimBlob(const blob_rec_t *b) {
    uint64_t end = (uint64_t)g.cfg->units * g.cfg->unit;
    bool adopted = (b->flags & BLOB_FLAG_ADOPTED) != 0;

    for (size_t i = 0; i < b->nExt; i++) {
        uint32_t first;
        uint32_t last;

        if (b->ext[i].length == 0 ||
            (uint64_t)b->ext[i].offset + b->ext[i].length > end)
            return false;
        first = b->ext[i].offset / g.cfg->unit;
        last = (b->ext[i].offset + b->ext[i].length - 1) / g.cfg->unit;
        for (uint32_t u = first; u <= last; u++) {
            if (unitUsed(u) && !(adopted && unitAdopted(u)))
                return false;
        }
    }
    for (size_t i = 0; i < b->nExt; i++)
        markRange(b->ext[i].offset, b->ext[i].length, true, adopted);
    return true;
}

static int visitRebuild(const char *name, void *arg) {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    blob_rec_t b;

    (void)arg;
    if (!fromHex(name, digest))
        return 0;
    if (loadBlob(digest, &b) < 0 || memcmp(b.digest, digest, sizeof(digest)) ||
        !claimBlob(&b)) {
        DEBUG_TRACE("registry: discarding blob record %s", name);
        RegFilesDel(g.cfg->blobDir, name);
    }
    return 0;
}

/* Creates `path` and the directories above it that are missing. */
static int makeDirs(const char *path) {
    char buf[DIR_PATH_MAX];
    size_t n = strlen(path);

    if (n == 0 || n >= sizeof(buf))
        return -ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        buf[i] = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST)
            return -errno;
        buf[i] = i < n ? '/' : '\0';
    }
    return 0;
}

static int _Init(registry_store_t *s) {
    int rc;

    (void)s;
    rc = makeDirs(g.cfg->entryDir);
    if (rc == 0)
        rc = makeDirs(g.cfg->blobDir);
    if (rc < 0)
        return rc;
    RegFilesDel(g.cfg->entryDir, STAGE_NAME);
    RegFilesDel(g.cfg->blobDir, STAGE_NAME);
    memset(g.refs, 0, sizeof(g.refs));
    memset(g.adopted, 0, sizeof(g.adopted));
    memset(&g.stage, 0, sizeof(g.stage));
    RegFilesList(g.cfg->blobDir, visitRebuild, NULL);
    return 0;
}

static const registry_store_ops_t ops = {
    .init = _Init,
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

registry_store_t *RegistryPageStore(const registry_page_config_t *cfg) {
    memset(&g, 0, sizeof(g));
    g.cfg = cfg;
    g.store.ops = &ops;
    return &g.store;
}

int RegistryPageAdopt(registry_store_t *s,
                      const uint8_t digest[REGISTRY_DIGEST_LEN],
                      uint32_t offset, uint32_t length) {
    char name[HEX_LEN + 1];
    uint8_t rec[BLOB_REC_MAX];
    blob_rec_t b;
    size_t n;
    int rc;

    (void)s;
    if (length == 0)
        return -EINVAL;
    if (loadBlob(digest, &b) == 0)
        return 0;
    memset(&b, 0, sizeof(b));
    memcpy(b.digest, digest, REGISTRY_DIGEST_LEN);
    b.length = length;
    b.flags = BLOB_FLAG_ADOPTED;
    b.nExt = 1;
    b.ext[0].offset = offset;
    b.ext[0].length = length;
    if (!claimBlob(&b))
        return -EEXIST;

    toHex(digest, name);
    n = encodeBlob(&b, rec);
    rc = barrier();
    if (rc == 0)
        rc = RegFilesPut(g.cfg->blobDir, name, rec, n);
    if (rc < 0)
        markRange(offset, length, false, true);
    return rc;
}

typedef struct {
    uint8_t *owners;
    registry_page_report_t *rep;
} check_ctx_t;

static int visitCheck(const char *name, void *arg) {
    uint8_t digest[REGISTRY_DIGEST_LEN];
    blob_rec_t b;
    check_ctx_t *c = arg;
    uint64_t end = (uint64_t)g.cfg->units * g.cfg->unit;

    if (!fromHex(name, digest) || loadBlob(digest, &b) < 0)
        return 0;
    for (size_t i = 0; i < b.nExt; i++) {
        uint32_t first;
        uint32_t last;

        if (b.ext[i].length == 0 ||
            (uint64_t)b.ext[i].offset + b.ext[i].length > end) {
            c->rep->outOfRange++;
            continue;
        }
        first = b.ext[i].offset / g.cfg->unit;
        last = (b.ext[i].offset + b.ext[i].length - 1) / g.cfg->unit;
        for (uint32_t u = first; u <= last; u++) {
            if (c->owners[u] < 255)
                c->owners[u]++;
        }
    }
    return 0;
}

int RegistryPageCheck(registry_store_t *s, registry_page_report_t *out) {
    check_ctx_t c;

    (void)s;
    memset(out, 0, sizeof(*out));
    c.owners = WantedMalloc(g.cfg->units);
    if (c.owners == NULL)
        return -ENOMEM;
    memset(c.owners, 0, g.cfg->units);
    c.rep = out;
    RegFilesList(g.cfg->blobDir, visitCheck, &c);
    for (uint32_t u = 0; u < g.cfg->units; u++) {
        if (c.owners[u] > 1)
            out->shared++;
        if ((c.owners[u] > 0) != unitUsed(u) && !g.stage.active)
            out->mismatch++;
    }
    out->units = g.cfg->units;
    out->freeUnits = countFree();
    WantedFree(c.owners);
    return 0;
}
