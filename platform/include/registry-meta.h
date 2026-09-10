/* SPDX-License-Identifier: Apache-2.0 */

/* Per-entry registry metadata every backend stores: what the stored bytes
 * hash to, and the signature binding them to the entry's name and version.
 * See docs/security-model.md for the signed message and the sidecar route. */

#ifndef REGISTRY_META_H
#define REGISTRY_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfs-tarfs.h>

#define REGISTRY_META_MAGIC 0x57415033u /* "WAP3" */
#define REGISTRY_META_DIGEST_LEN 32
#define REGISTRY_META_SIG_LEN 64
#define REGISTRY_META_MAX_LAYERS TARFS_MAX_LAYERS

/* Firmware-carried image, covered by the firmware signature. */
#define REGISTRY_META_SEEDED 0x01u
/* The record carries a signature and a key id. */
#define REGISTRY_META_SIGNED 0x02u

/* Layer digests run base-first, matching the order the signed message binds
 * them in; wapp_t.layers[] runs topmost-first and needs reversing. */
typedef struct {
    uint32_t magic;
    uint32_t size; /* stored image length */
    uint32_t keyId;
    uint8_t layerCount;
    uint8_t flags;
    uint8_t reserved[2];
    uint8_t layerDigest[REGISTRY_META_MAX_LAYERS][REGISTRY_META_DIGEST_LEN];
    uint8_t signature[REGISTRY_META_SIG_LEN];
} registry_meta_t;

/* The record outlives the firmware that wrote it, so a shorter or older one
 * must not read as one carrying every field. */
static inline bool RegistryMetaValid(const registry_meta_t *meta,
                                     size_t bytes) {
    return meta != NULL && bytes >= sizeof(*meta) &&
           meta->magic == REGISTRY_META_MAGIC &&
           meta->layerCount <= REGISTRY_META_MAX_LAYERS;
}

/* Payload the `.sig` route accepts: a big-endian key id then the raw
 * signature. Fixed-width, so the engine parses no wire encoding. */
#define REGISTRY_SIG_PAYLOAD_LEN (4 + REGISTRY_META_SIG_LEN)

#endif /* REGISTRY_META_H */
