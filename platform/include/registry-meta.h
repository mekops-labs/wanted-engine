/* SPDX-License-Identifier: Apache-2.0 */

/* Per-entry registry record: the layers an image holds, what each hashes to,
 * when it was installed, and the signature binding them to a name and version.
 * See docs/security-model.md for the signed message and the signature route. */

#ifndef REGISTRY_META_H
#define REGISTRY_META_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfs-tarfs.h>

#define REGISTRY_META_MAGIC 0x57415034u    /* "WAP4" */
#define REGISTRY_META_MAGIC_V3 0x57415033u /* "WAP3": one layer, no times */
#define REGISTRY_META_DIGEST_LEN 32
/* A digest as lowercase hex, without the NUL. */
#define REGISTRY_META_DIGEST_HEX_LEN 64
#define REGISTRY_META_SIG_LEN 64
#define REGISTRY_META_MAX_LAYERS TARFS_MAX_LAYERS

/* Firmware-carried image, covered by the firmware signature. */
#define REGISTRY_META_SEEDED 0x01u
/* The record carries a signature and a key id. */
#define REGISTRY_META_SIGNED 0x02u
/* The clock was not valid at install, so `installed` is 0. */
#define REGISTRY_META_CLOCK_UNSET 0x04u

/* Layer arrays run base-first, as the signed message binds them; wapp_t.layers
 * runs topmost-first. `installSeq` orders installs, the highest being newest.
 * `installed` and `created` are Unix seconds, 0 when unknown. */
typedef struct {
    uint32_t magic;
    uint32_t size; /* total of the layer lengths */
    uint32_t keyId;
    uint32_t installSeq;
    int64_t installed;
    int64_t created;
    uint8_t layerCount;
    uint8_t flags;
    uint8_t reserved[2];
    uint32_t layerLen[REGISTRY_META_MAX_LAYERS];
    uint8_t layerDigest[REGISTRY_META_MAX_LAYERS][REGISTRY_META_DIGEST_LEN];
    uint8_t signature[REGISTRY_META_SIG_LEN];
} registry_meta_t;

/* Stored form, independent of the layer limit a build was configured with. */
#define REGISTRY_RECORD_HEADER_LEN 36
#define REGISTRY_RECORD_LAYER_LEN (4 + REGISTRY_META_DIGEST_LEN)
#define REGISTRY_RECORD_ABS_MAX_LAYERS 16
#define REGISTRY_RECORD_MAX                                                    \
    (REGISTRY_RECORD_HEADER_LEN +                                              \
     (REGISTRY_RECORD_ABS_MAX_LAYERS * REGISTRY_RECORD_LAYER_LEN) +            \
     REGISTRY_META_SIG_LEN)

/* Bytes written, or 0 when `cap` is too small or the layer count is out of
 * range. */
size_t RegistryRecordEncode(const registry_meta_t *meta, uint8_t *out,
                            size_t cap);
/* False for a short record, an unknown magic, or more layers than this build
 * holds. A WAP3 record reads as one layer with no times. */
bool RegistryRecordDecode(const uint8_t *in, size_t len, registry_meta_t *meta);

/* Payload the `.sig` route accepts: a big-endian key id then the raw
 * signature. Fixed-width, so the engine parses no wire encoding. */
#define REGISTRY_SIG_PAYLOAD_LEN (4 + REGISTRY_META_SIG_LEN)

#endif /* REGISTRY_META_H */
