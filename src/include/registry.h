/* SPDX-License-Identifier: Apache-2.0 */

/* The registry: the wapp images installed on a device. Every rule about refs,
 * layers, commits, versions and verification records lives here, above the
 * store interface of registry-store.h. */

#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <platform.h>
#include <registry-meta.h>
#include <registry-store.h>
#include <wanted-api.h>

/* Entries enumerated and resolved; a longer registry is truncated. */
#define REGISTRY_MAX_ENTRIES 50
/* Stores the registry runs on at once. */
#define REGISTRY_MAX_STORES 3

/* Lists up to `len` entries sorted by name then version and returns the total
 * count, or a negative errno. A NULL list only counts. */
int RegistryRead(reg_entry_t *list, size_t len);

/* A stream: START_WRITE names the ref and carries the first chunk, then
 * CONTINUE_WRITE, then FINISH_WRITE or ABORT_WRITE. RegistryWrite commits a
 * one-layer image at the end; RegistryWriteLayer commits nothing. */
int RegistryWrite(write_state_t s, const char *ref, const uint8_t *buf,
                  size_t nbytes);
int RegistryWriteLayer(write_state_t s, const char *ref, unsigned layer,
                       const uint8_t *buf, size_t nbytes);

/* Publish `ref` from the layers 0..count-1 staged for it. `created` is the
 * image build time, read only when `hasCreated`. -ENOENT names a missing
 * layer, -E2BIG a count above the layer limit, -EINVAL a count of 0. */
int RegistryCommit(const char *ref, unsigned count, bool hasCreated,
                   int64_t created);

/* Removes the entry; -EPERM for a firmware-provided one. Its blobs are freed
 * once no entry lists them and no wapp maps them. */
int RegistryRemove(const reg_entry_t *entry);

/* Maps every layer of the entry, topmost first, into `w` and names the image.
 * A bare name takes the newest entry that carries it. */
int RegistryLoad(const reg_entry_t *entry, wapp_t *w);
/* Releases the layers RegistryLoad mapped. False when `w` holds none. */
bool RegistryUnload(const wapp_t *w);

/* The same mapping without a wapp, for reading an image. */
int RegistryMapLayers(const reg_entry_t *entry,
                      uint8_t *layers[REGISTRY_META_MAX_LAYERS],
                      size_t lens[REGISTRY_META_MAX_LAYERS], uint8_t *count);
void RegistryUnmapLayers(uint8_t *const layers[], uint8_t count);

/* The entry's record. A bare name takes the newest entry. -ENOENT where there
 * is none. */
int RegistryMetaRead(const reg_entry_t *entry, registry_meta_t *out);
/* Attaches a key id and signature to a committed entry. */
int RegistryMetaSetSignature(const reg_entry_t *entry, uint32_t keyId,
                             const uint8_t sig[REGISTRY_META_SIG_LEN]);
/* Whether a running wapp maps a layer of the entry. */
bool RegistryIsMapped(const reg_entry_t *entry);

/* Marks `ref` as firmware-provided, so RegistryRemove refuses it. */
void RegistryMarkSeeded(const char *ref);

/* Lists the one-layer image `data` as `ref`, from memory that stays mapped. It
 * holds no store space and cannot be removed; an installed entry of the same
 * ref takes its place. `created` is the build time in Unix seconds, or 0. */
int RegistryAddResident(const char *ref, const uint8_t *data, size_t len,
                        int64_t created);
/* Forgets every resident image. */
void RegistryClearResident(void);

/* One object for each store, up to `max`. Returns the store count. */
int RegistryCapacity(registry_capacity_t *out, size_t max);

/* A blob that a committed or resident entry lists and a store holds. */
typedef struct {
    uint32_t size;
    uint16_t entries; /* committed or resident entries that list it */
    bool mapped;
    bool resident;
    char store[REGISTRY_STORE_NAME_LEN];
} registry_blob_info_t;

/* The `n`th held blob, counted from 0, or -ENOENT past the last. The order
 * follows the entries and is stable while none is installed or removed. */
int RegistryBlobAt(size_t n, uint8_t digest[REGISTRY_DIGEST_LEN]);
/* Describes the blob, or -ENOENT when no entry lists it or no store holds it.
 */
int RegistryBlobInfo(const uint8_t digest[REGISTRY_DIGEST_LEN],
                     registry_blob_info_t *out);
/* Stages layer `layer` of `ref` as the blob, with no bytes written. -ENOENT
 * where RegistryBlobInfo finds none, -EINVAL for an index past the limit. */
int RegistryLinkLayer(const char *ref, unsigned layer,
                      const uint8_t digest[REGISTRY_DIGEST_LEN]);

/* Drops the stage, the staged layers and the mappings, and runs the store
 * discovery again at the next call. */
void RegistryDeinit(void);

#endif /* REGISTRY_H */
