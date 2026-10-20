/* SPDX-License-Identifier: Apache-2.0 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

/* The target's own config header: a platform on a dedicated filesystem
 * redefines REGISTRY_ROOT there, and a board has no working directory for
 * the generic header's relative default to resolve against. */
#ifdef __NuttX__
#include <config-nuttx.h>
#else
#include <platform-config.h>
#endif

#include <platform.h>
#include <registry-files.h>
#include <registry-fs.h>
#include <wanted-api.h>

#define ENTRIES_DIR REGISTRY_ROOT "/entries"
#define BLOBS_DIR REGISTRY_ROOT "/blobs"
#define STAGE_PATH REGISTRY_ROOT "/_stage"
#define REC_TEMP REGISTRY_ROOT "/_rec"
#define HEX_LEN REGISTRY_META_DIGEST_HEX_LEN
#define SCAN_MAX 64
#define READ_CHUNK 512

static registry_store_t store;

static int blobPath(char *out, size_t cap, const uint8_t *digest) {
    char hex[HEX_LEN + 1];
    int n;

    RegistryDigestToHex(digest, hex);
    n = snprintf(out, cap, "%s/%s", BLOBS_DIR, hex);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static int recPath(char *out, size_t cap, const registry_key_t *key) {
    char file[NAME_MAX + 1];
    int rc = RegFilesKeyName(file, sizeof(file), key);
    int n;

    if (rc < 0)
        return rc;
    n = snprintf(out, cap, "%s/%s", ENTRIES_DIR, file);
    return n < 0 || (size_t)n >= cap ? -ENAMETOOLONG : 0;
}

static bool exists(const char *path) {
    struct stat st;

    return stat(path, &st) == 0;
}

/* A file lands whole or not at all: staged, then renamed over the target. */
static int writeWhole(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(REC_TEMP, "wb");
    size_t w;
    int closed;

    if (f == NULL)
        return -errno;
    w = fwrite(data, 1, len, f);
    closed = fclose(f);
    if (w != len || closed != 0) {
        remove(REC_TEMP);
        return -EIO;
    }
    if (rename(REC_TEMP, path) < 0) {
        int err = -errno;

        remove(REC_TEMP);
        return err;
    }
    return 0;
}

static int _RecPut(registry_store_t *s, const registry_key_t *key,
                   const uint8_t *rec, size_t len) {
    char path[PATH_MAX];
    int rc = recPath(path, sizeof(path), key);

    (void)s;
    return rc < 0 ? rc : writeWhole(path, rec, len);
}

static int _RecGet(registry_store_t *s, const registry_key_t *key, uint8_t *rec,
                   size_t cap) {
    char path[PATH_MAX];
    FILE *f;
    size_t r;
    int rc = recPath(path, sizeof(path), key);

    (void)s;
    if (rc < 0)
        return rc;
    f = fopen(path, "rb");
    if (f == NULL)
        return -ENOENT;
    r = fread(rec, 1, cap, f);
    rc = fgetc(f) != EOF ? -EMSGSIZE : (int)r;
    fclose(f);
    return rc;
}

static int _RecDel(registry_store_t *s, const registry_key_t *key) {
    char path[PATH_MAX];
    int rc = recPath(path, sizeof(path), key);

    (void)s;
    if (rc < 0)
        return rc;
    return remove(path) == 0 ? 0 : -errno;
}

static int listRaw(registry_key_t *keys, size_t max) {
    DIR *dir = opendir(ENTRIES_DIR);
    const struct dirent *de;
    registry_key_t key;
    int n = 0;

    if (dir == NULL)
        return errno == ENOENT ? 0 : -errno;
    while ((de = readdir(dir)) != NULL) {
        if (!RegFilesParseKey(de->d_name, &key))
            continue;
        if ((size_t)n < max)
            keys[n] = key;
        n++;
    }
    closedir(dir);
    return n;
}

static int _StageBegin(registry_store_t *s, void **stage) {
    FILE *f = fopen(STAGE_PATH, "wb");

    (void)s;
    if (f == NULL)
        return -errno;
    *stage = f;
    return 0;
}

static int _StageWrite(registry_store_t *s, void *stage, const uint8_t *buf,
                       size_t n) {
    size_t w = fwrite(buf, 1, n, (FILE *)stage);

    (void)s;
    if (w != n)
        return errno == ENOSPC || errno == 0 ? -ENOSPC : -errno;
    return (int)n;
}

static int _StageEnd(registry_store_t *s, void *stage,
                     const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len) {
    char path[PATH_MAX];
    int rc = blobPath(path, sizeof(path), digest);
    int closed = fclose((FILE *)stage);

    (void)s;
    (void)len;
    if (closed != 0) {
        remove(STAGE_PATH);
        return -EIO;
    }
    if (rc < 0 || exists(path)) {
        remove(STAGE_PATH);
        return rc;
    }
    if (rename(STAGE_PATH, path) < 0) {
        rc = -errno;
        remove(STAGE_PATH);
        return rc;
    }
    return 0;
}

static void _StageAbort(registry_store_t *s, void *stage) {
    (void)s;
    fclose((FILE *)stage);
    remove(STAGE_PATH);
}

static int _BlobMap(registry_store_t *s,
                    const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len,
                    registry_blob_map_t *out) {
    char path[PATH_MAX];
    uint8_t *ptr;
    size_t mapped;
    int rc = blobPath(path, sizeof(path), digest);

    (void)s;
    (void)len;
    if (rc < 0)
        return rc;
    if (!exists(path))
        return -ENOENT;
    rc = PlatformFileMap(path, &ptr, &mapped);
    if (rc < 0)
        return rc;
    out->ptr = ptr;
    out->len = mapped;
    out->handle = NULL;
    return 0;
}

static void _BlobUnmap(registry_store_t *s, registry_blob_map_t *map) {
    (void)s;
    PlatformFileUnmap((uint8_t *)map->ptr, map->len);
}

static bool _BlobHas(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    char path[PATH_MAX];

    (void)s;
    return blobPath(path, sizeof(path), digest) == 0 && exists(path);
}

static int _BlobDrop(registry_store_t *s,
                     const uint8_t digest[REGISTRY_DIGEST_LEN]) {
    char path[PATH_MAX];
    int rc = blobPath(path, sizeof(path), digest);

    (void)s;
    if (rc < 0)
        return rc;
    return remove(path) == 0 ? 0 : -errno;
}

static int _BlobList(registry_store_t *s,
                     uint8_t (*digests)[REGISTRY_DIGEST_LEN], size_t max) {
    DIR *dir = opendir(BLOBS_DIR);
    const struct dirent *de;
    uint8_t digest[REGISTRY_DIGEST_LEN];
    int n = 0;

    (void)s;
    if (dir == NULL)
        return errno == ENOENT ? 0 : -errno;
    while ((de = readdir(dir)) != NULL) {
        if (RegistryDigestFromHex(de->d_name, strlen(de->d_name), digest) < 0)
            continue;
        if ((size_t)n < max)
            memcpy(digests[n], digest, REGISTRY_DIGEST_LEN);
        n++;
    }
    closedir(dir);
    return n;
}

static int _Capacity(registry_store_t *s, registry_capacity_t *out) {
    size_t freeB = 0;
    size_t totalB = 0;

    (void)s;
    PlatformStorageStats(&freeB, &totalB);
    strncpy(out->name, "files", sizeof(out->name) - 1);
    strncpy(out->kind, "file", sizeof(out->kind) - 1);
    out->total = (uint32_t)totalB;
    out->freeBytes = (uint32_t)freeB;
    out->largestFree = (uint32_t)freeB;
    out->caps = REGISTRY_CAP_MAPPABLE | REGISTRY_CAP_SCATTERED;
    out->present = true;
    return 0;
}

/* The layout before blobs: "<name><sep><version>.wapp" with a ".meta" record
 * beside it. Each such image becomes a blob and an entry record. */
static bool oldImageKey(const char *file, registry_key_t *key) {
    size_t len = strlen(file);
    size_t extLen = sizeof(REGISTRY_EXT) - 1;
    const char *sep;
    size_t nameLen;
    size_t verLen = 0;

    if (len <= extLen || strcmp(file + len - extLen, REGISTRY_EXT) != 0)
        return false;
    sep = strchr(file, REGISTRY_VERSION_SEPARATOR);
    nameLen = sep != NULL ? (size_t)(sep - file) : len - extLen;
    if (sep != NULL)
        verLen = len - extLen - nameLen - 1;
    if (nameLen == 0 || nameLen >= REGISTRY_KEY_NAME_LEN ||
        verLen >= REGISTRY_KEY_NAME_LEN)
        return false;

    memset(key, 0, sizeof(*key));
    memcpy(key->name, file, nameLen);
    if (sep != NULL)
        memcpy(key->version, sep + 1, verLen);
    return true;
}

static bool hashFile(const char *path, uint8_t digest[REGISTRY_DIGEST_LEN],
                     size_t *size) {
    uint8_t chunk[READ_CHUNK];
    void *sha = PlatformSha256New();
    FILE *f = fopen(path, "rb");
    size_t r;

    *size = 0;
    if (f == NULL || sha == NULL) {
        if (f != NULL)
            fclose(f);
        if (sha != NULL)
            PlatformSha256Free(sha);
        return false;
    }
    while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        PlatformSha256Update(sha, chunk, r);
        *size += r;
    }
    fclose(f);
    PlatformSha256Final(sha, digest);
    PlatformSha256Free(sha);
    return true;
}

static void oldMeta(const char *base, registry_meta_t *meta) {
    char path[PATH_MAX];
    uint8_t rec[REGISTRY_RECORD_MAX];
    FILE *f;
    size_t r;

    memset(meta, 0, sizeof(*meta));
    snprintf(path, sizeof(path), "%s%s", base, REGISTRY_META_EXT);
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    r = fread(rec, 1, sizeof(rec), f);
    fclose(f);
    if (!RegistryRecordDecode(rec, r, meta))
        memset(meta, 0, sizeof(*meta));
    remove(path);
}

static bool convertOld(const char *file, uint32_t seq) {
    char path[PATH_MAX];
    char base[PATH_MAX];
    char dest[PATH_MAX];
    uint8_t rec[REGISTRY_RECORD_MAX];
    registry_meta_t meta;
    registry_key_t key;
    size_t size;
    size_t n;

    if (!oldImageKey(file, &key))
        return false;
    snprintf(path, sizeof(path), "%s/%s", REGISTRY_ROOT, file);
    snprintf(base, sizeof(base), "%s/%.*s", REGISTRY_ROOT,
             (int)(strlen(file) - (sizeof(REGISTRY_EXT) - 1)), file);

    oldMeta(base, &meta);
    if (!hashFile(path, meta.layerDigest[0], &size) || size == 0)
        return false;
    if (blobPath(dest, sizeof(dest), meta.layerDigest[0]) < 0)
        return false;
    if (exists(dest))
        remove(path);
    else if (rename(path, dest) < 0)
        return false;

    meta.magic = REGISTRY_META_MAGIC;
    meta.layerCount = 1;
    meta.layerLen[0] = (uint32_t)size;
    meta.size = (uint32_t)size;
    meta.installSeq = seq;
    meta.flags |= REGISTRY_META_CLOCK_UNSET;
    n = RegistryRecordEncode(&meta, rec, sizeof(rec));
    return n > 0 && _RecPut(&store, &key, rec, n) == 0;
}

/* The highest install sequence any entry record carries. */
static uint32_t maxSeq(void) {
    /* Static: conversion runs at init or under the registry lock. */
    static registry_key_t keys[SCAN_MAX];
    uint8_t rec[REGISTRY_RECORD_MAX];
    registry_meta_t meta;
    uint32_t top = 0;
    int n = listRaw(keys, SCAN_MAX);

    if (n > SCAN_MAX)
        n = SCAN_MAX;
    for (int i = 0; i < n; i++) {
        int got = _RecGet(&store, &keys[i], rec, sizeof(rec));

        if (got > 0 && RegistryRecordDecode(rec, (size_t)got, &meta) &&
            meta.installSeq > top)
            top = meta.installSeq;
    }
    return top;
}

/* Converts every old-layout image in the root, in batches since a conversion
 * changes the directory it walks. A file dropped into the root later is
 * converted the same way, as the newest entry. */
static void convertAll(void) {
    char files[8][NAME_MAX + 1];
    size_t converted;

    do {
        DIR *dir = opendir(REGISTRY_ROOT);
        const struct dirent *de;
        size_t nFiles = 0;

        converted = 0;
        if (dir == NULL)
            return;
        while ((de = readdir(dir)) != NULL && nFiles < 8) {
            registry_key_t key;

            if (oldImageKey(de->d_name, &key)) {
                strncpy(files[nFiles], de->d_name, NAME_MAX);
                files[nFiles][NAME_MAX] = '\0';
                nFiles++;
            }
        }
        closedir(dir);
        for (size_t i = 0; i < nFiles; i++) {
            if (convertOld(files[i], maxSeq() + 1))
                converted++;
        }
    } while (converted > 0);
}

static int _RecList(registry_store_t *s, registry_key_t *keys, size_t max) {
    (void)s;
    convertAll();
    return listRaw(keys, max);
}

static int _Init(registry_store_t *s) {
    (void)s;
    if (mkdir(REGISTRY_ROOT, 0755) < 0 && errno != EEXIST)
        return -errno;
    if (mkdir(ENTRIES_DIR, 0755) < 0 && errno != EEXIST)
        return -errno;
    if (mkdir(BLOBS_DIR, 0755) < 0 && errno != EEXIST)
        return -errno;
    remove(STAGE_PATH);
    remove(REC_TEMP);
    convertAll();
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

registry_store_t *RegistryFsStore(void) {
    store.ops = &ops;
    return &store;
}
