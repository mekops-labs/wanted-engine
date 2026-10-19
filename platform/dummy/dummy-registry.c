/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <string.h>

#include <platform.h>
#include <registry-mem.h>
#include <registry.h>

#include "dummy-fs.h"

/* The dummy registry is the in-memory store behind the registry core. Tests
 * fill it with DummyRegistrySeed, which stores a canned image under each ref.
 */

#define DUMMY_REG_MAX_ENTRIES 8
#define TAR_BLOCK 512
#define TAR_SIZE_OFFSET 124
#define TAR_TYPE_OFFSET 156
#define TAR_MAGIC_OFFSET 257
#define OCTAL_DIGITS 11

/* The module is `(memory 1 4)`. */
static const uint8_t cannedWasm[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00,
                                     0x00, 0x05, 0x04, 0x01, 0x01, 0x01, 0x04};

/* One app.wasm member, then the two zero blocks that end the archive. */
#define CANNED_IMAGE_LEN (4 * TAR_BLOCK)

static void buildCannedImage(uint8_t image[CANNED_IMAGE_LEN]) {
    size_t size = sizeof(cannedWasm);

    memset(image, 0, CANNED_IMAGE_LEN);
    memcpy(image, "app.wasm", 8);
    for (int i = OCTAL_DIGITS - 1; i >= 0; i--) {
        image[TAR_SIZE_OFFSET + i] = (uint8_t)('0' + (size & 7));
        size >>= 3;
    }
    image[TAR_TYPE_OFFSET] = '0';
    memcpy(image + TAR_MAGIC_OFFSET, "ustar", 5);
    memcpy(image + TAR_BLOCK, cannedWasm, sizeof(cannedWasm));
}

static uint32_t seedSeq;

static void keyOf(registry_key_t *key, const char *name, const char *version) {
    memset(key, 0, sizeof(*key));
    strncpy(key->name, name, REGISTRY_KEY_NAME_LEN - 1);
    strncpy(key->version, version, REGISTRY_KEY_NAME_LEN - 1);
}

static int findByName(const char *name, registry_key_t *out) {
    registry_store_t *s = RegistryMemStore();
    registry_key_t keys[REGISTRY_MAX_ENTRIES];
    int n = s->ops->recList(s, keys, REGISTRY_MAX_ENTRIES);

    for (int i = 0; i < n && i < REGISTRY_MAX_ENTRIES; i++) {
        if (strncmp(keys[i].name, name, REGISTRY_KEY_NAME_LEN) == 0) {
            *out = keys[i];
            return 0;
        }
    }
    return -ENOENT;
}

static int putRecord(const registry_key_t *key, const registry_meta_t *meta) {
    registry_store_t *s = RegistryMemStore();
    uint8_t rec[REGISTRY_RECORD_MAX];
    size_t n = RegistryRecordEncode(meta, rec, sizeof(rec));

    return n == 0 ? -EINVAL : s->ops->recPut(s, key, rec, n);
}

/* Stores the canned image and returns its digest. */
static int storeImage(uint8_t digest[REGISTRY_DIGEST_LEN]) {
    registry_store_t *s = RegistryMemStore();
    uint8_t image[CANNED_IMAGE_LEN];
    void *stage = NULL;
    void *sha = PlatformSha256New();
    int rc;

    if (sha == NULL)
        return -ENOMEM;
    buildCannedImage(image);
    PlatformSha256Update(sha, image, sizeof(image));
    PlatformSha256Final(sha, digest);
    PlatformSha256Free(sha);

    rc = s->ops->stageBegin(s, &stage);
    if (rc < 0)
        return rc;
    rc = s->ops->stageWrite(s, stage, image, sizeof(image));
    if (rc < 0) {
        s->ops->stageAbort(s, stage);
        return rc;
    }
    return s->ops->stageEnd(s, stage, digest, sizeof(image));
}

/* ── Test control ───────────────────────────────────────────────────────── */

void DummyRegistryReset(void) {
    RegistryDeinit();
    RegistryClearResident();
    RegistryMemReset();
    seedSeq = 0;
}

int DummyRegistrySeed(const reg_entry_t *entries, size_t count) {
    registry_store_t *s = RegistryMemStore();
    int stored = 0;

    if (!entries)
        return -EINVAL;

    for (size_t i = 0; i < count; i++) {
        registry_key_t key;
        registry_key_t old;
        registry_meta_t meta;
        bool upsert = findByName(entries[i].name, &old) == 0;
        int rc;

        if (!upsert && s->ops->recList(s, &old, 0) >= DUMMY_REG_MAX_ENTRIES)
            return -ENOSPC;
        if (upsert)
            s->ops->recDel(s, &old);

        memset(&meta, 0, sizeof(meta));
        meta.magic = REGISTRY_META_MAGIC;
        meta.layerCount = 1;
        meta.size = (uint32_t)entries[i].size;
        meta.layerLen[0] = CANNED_IMAGE_LEN;
        meta.installSeq = ++seedSeq;
        rc = storeImage(meta.layerDigest[0]);
        if (rc < 0)
            return rc;
        keyOf(&key, entries[i].name, entries[i].version);
        rc = putRecord(&key, &meta);
        if (rc < 0)
            return rc;
        stored++;
    }
    return stored;
}

/* Replace an entry's record, so a test can pose an image as signed, seeded, or
 * hashing to something other than its bytes. */
int DummyRegistrySetMeta(const reg_entry_t *entry,
                         const registry_meta_t *meta) {
    registry_key_t key;

    if (!entry || !meta)
        return -EINVAL;
    if (findByName(entry->name, &key) < 0)
        return -ENOENT;
    return putRecord(&key, meta);
}

/* Drop an entry's record, which removes the entry the record describes. */
int DummyRegistryDropMeta(const reg_entry_t *entry) {
    registry_store_t *s = RegistryMemStore();
    registry_key_t key;

    if (!entry)
        return -EINVAL;
    if (findByName(entry->name, &key) < 0)
        return -ENOENT;
    return s->ops->recDel(s, &key);
}

static registry_store_t *override;

void DummyRegistryUseStore(registry_store_t *store) {
    RegistryDeinit();
    override = store;
}

size_t PlatformRegistryStores(registry_store_t **out, size_t max) {
    if (max == 0)
        return 0;
    out[0] = override != NULL ? override : RegistryMemStore();
    return 1;
}
