/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <registry-meta.h>

/* Little-endian, byte by byte, so a record reads the same on any target. */
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

static void put64(uint8_t *p, int64_t v) {
    put32(p, (uint32_t)((uint64_t)v & 0xffffffffU));
    put32(p + 4, (uint32_t)((uint64_t)v >> 32));
}

static int64_t get64(const uint8_t *p) {
    return (int64_t)((uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32));
}

/* Layout: magic u32, flags u8, layers u8, reserved u16, key id u32, sequence
 * u32, size u32, installed i64, created i64, then per layer a length u32 and
 * a digest, then the signature. */
size_t RegistryRecordEncode(const registry_meta_t *meta, uint8_t *out,
                            size_t cap) {
    size_t need;
    size_t off = REGISTRY_RECORD_HEADER_LEN;

    if (meta == NULL || out == NULL ||
        meta->layerCount > REGISTRY_META_MAX_LAYERS)
        return 0;
    need = REGISTRY_RECORD_HEADER_LEN +
           ((size_t)meta->layerCount * REGISTRY_RECORD_LAYER_LEN) +
           REGISTRY_META_SIG_LEN;
    if (cap < need)
        return 0;

    memset(out, 0, need);
    put32(out, REGISTRY_META_MAGIC);
    out[4] = meta->flags;
    out[5] = meta->layerCount;
    put32(out + 8, meta->keyId);
    put32(out + 12, meta->installSeq);
    put32(out + 16, meta->size);
    put64(out + 20, meta->installed);
    put64(out + 28, meta->created);
    for (size_t i = 0; i < meta->layerCount; i++) {
        put32(out + off, meta->layerLen[i]);
        memcpy(out + off + 4, meta->layerDigest[i], REGISTRY_META_DIGEST_LEN);
        off += REGISTRY_RECORD_LAYER_LEN;
    }
    memcpy(out + off, meta->signature, REGISTRY_META_SIG_LEN);
    return need;
}

/* The WAP3 record was a structure dump: magic, size, key id, layer count,
 * flags, two pad bytes, a digest for each layer slot, then the signature. */
static bool decodeV3(const uint8_t *in, size_t len, registry_meta_t *meta) {
    const size_t slots = REGISTRY_META_MAX_LAYERS;
    const size_t need =
        16 + (slots * REGISTRY_META_DIGEST_LEN) + REGISTRY_META_SIG_LEN;
    uint8_t count;

    if (len < need)
        return false;
    count = in[12];
    if (count > slots)
        return false;

    memset(meta, 0, sizeof(*meta));
    meta->magic = REGISTRY_META_MAGIC;
    meta->size = get32(in + 4);
    meta->keyId = get32(in + 8);
    meta->layerCount = count;
    meta->flags = (uint8_t)(in[13] | REGISTRY_META_CLOCK_UNSET);
    for (size_t i = 0; i < count; i++)
        memcpy(meta->layerDigest[i], in + 16 + (i * REGISTRY_META_DIGEST_LEN),
               REGISTRY_META_DIGEST_LEN);
    if (count == 1)
        meta->layerLen[0] = meta->size;
    memcpy(meta->signature, in + 16 + (slots * REGISTRY_META_DIGEST_LEN),
           REGISTRY_META_SIG_LEN);
    return true;
}

bool RegistryRecordDecode(const uint8_t *in, size_t len,
                          registry_meta_t *meta) {
    size_t off = REGISTRY_RECORD_HEADER_LEN;
    size_t need;
    uint8_t count;

    if (in == NULL || meta == NULL || len < 4)
        return false;
    if (get32(in) == REGISTRY_META_MAGIC_V3)
        return decodeV3(in, len, meta);
    if (get32(in) != REGISTRY_META_MAGIC || len < REGISTRY_RECORD_HEADER_LEN)
        return false;

    count = in[5];
    if (count > REGISTRY_META_MAX_LAYERS)
        return false;
    need = REGISTRY_RECORD_HEADER_LEN +
           ((size_t)count * REGISTRY_RECORD_LAYER_LEN) + REGISTRY_META_SIG_LEN;
    if (len < need)
        return false;

    memset(meta, 0, sizeof(*meta));
    meta->magic = REGISTRY_META_MAGIC;
    meta->flags = in[4];
    meta->layerCount = count;
    meta->keyId = get32(in + 8);
    meta->installSeq = get32(in + 12);
    meta->size = get32(in + 16);
    meta->installed = get64(in + 20);
    meta->created = get64(in + 28);
    for (size_t i = 0; i < count; i++) {
        meta->layerLen[i] = get32(in + off);
        memcpy(meta->layerDigest[i], in + off + 4, REGISTRY_META_DIGEST_LEN);
        off += REGISTRY_RECORD_LAYER_LEN;
    }
    memcpy(meta->signature, in + off, REGISTRY_META_SIG_LEN);
    return true;
}
