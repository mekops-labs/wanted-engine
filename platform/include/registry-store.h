/* SPDX-License-Identifier: Apache-2.0 */

/* What a storage backing implements for the registry: blobs keyed by the
 * SHA-256 of their bytes, and optionally the entry records that list them. The
 * rules about refs, versions and images live above it, in src/registry.c. */

#ifndef REGISTRY_STORE_H
#define REGISTRY_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <registry-meta.h>

#define REGISTRY_DIGEST_LEN REGISTRY_META_DIGEST_LEN
#define REGISTRY_KEY_NAME_LEN 16
#define REGISTRY_STORE_NAME_LEN 16

/* The store can map a blob as one contiguous read-only range. */
#define REGISTRY_CAP_MAPPABLE 0x01u
/* The store can map non-adjacent units as one range. */
#define REGISTRY_CAP_SCATTERED 0x02u
/* The medium can be taken away at run time. */
#define REGISTRY_CAP_REMOVABLE 0x04u

typedef struct {
    char name[REGISTRY_KEY_NAME_LEN];
    char version[REGISTRY_KEY_NAME_LEN];
} registry_key_t;

typedef struct {
    const uint8_t *ptr;
    size_t len;
    void *handle; /* the store's own; returned to blobUnmap */
} registry_blob_map_t;

typedef struct {
    char name[REGISTRY_STORE_NAME_LEN];
    char kind[REGISTRY_STORE_NAME_LEN];
    uint32_t unit;        /* allocation unit in bytes, 0 where there is none */
    uint32_t total;       /* bytes the store can hold, 0 where unbounded */
    uint32_t freeBytes;   /* bytes still free */
    uint32_t largestFree; /* longest contiguous free run in bytes */
    uint8_t caps;
    bool present;
} registry_capacity_t;

typedef struct registry_store registry_store_t;

typedef struct {
    /* Rebuild what the store keeps in memory and finish any conversion of an
     * older layout. Called once, before the first other call. May be NULL. */
    int (*init)(registry_store_t *s);

    /* One stage at a time. stageEnd keeps the blob under `digest`, or drops
     * the copy when the store already holds that digest. stageWrite returns
     * the bytes taken or a negative errno. */
    int (*stageBegin)(registry_store_t *s, void **stage);
    int (*stageWrite)(registry_store_t *s, void *stage, const uint8_t *buf,
                      size_t n);
    int (*stageEnd)(registry_store_t *s, void *stage,
                    const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len);
    void (*stageAbort)(registry_store_t *s, void *stage);

    /* Map or release a whole blob. A mapped blob is never freed by drop. */
    int (*blobMap)(registry_store_t *s,
                   const uint8_t digest[REGISTRY_DIGEST_LEN], size_t len,
                   registry_blob_map_t *out);
    void (*blobUnmap)(registry_store_t *s, registry_blob_map_t *map);
    bool (*blobHas)(registry_store_t *s,
                    const uint8_t digest[REGISTRY_DIGEST_LEN]);
    int (*blobDrop)(registry_store_t *s,
                    const uint8_t digest[REGISTRY_DIGEST_LEN]);
    /* Fills up to `max` digests and returns how many the store holds. */
    int (*blobList)(registry_store_t *s,
                    uint8_t (*digests)[REGISTRY_DIGEST_LEN], size_t max);

    /* Entry records. Each put replaces the record whole or not at all. The
     * three calls are NULL on a store that holds blobs only. */
    int (*recPut)(registry_store_t *s, const registry_key_t *key,
                  const uint8_t *rec, size_t len);
    int (*recGet)(registry_store_t *s, const registry_key_t *key, uint8_t *rec,
                  size_t cap);
    int (*recDel)(registry_store_t *s, const registry_key_t *key);
    int (*recList)(registry_store_t *s, registry_key_t *keys, size_t max);

    int (*capacity)(registry_store_t *s, registry_capacity_t *out);
} registry_store_ops_t;

struct registry_store {
    const registry_store_ops_t *ops;
    void *ctx;
};

/* The stores of this platform, the first holding the entry records. Returns
 * how many were written to `out`. */
size_t PlatformRegistryStores(registry_store_t **out, size_t max);

#endif /* REGISTRY_STORE_H */
