/* SPDX-License-Identifier: Apache-2.0 */

/* Verify a registry image against its metadata record and the signing keyring
 * the firmware carries, on every load. See docs/security-model.md. */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <debug_trace.h>
#include <platform.h>
#include <wanted-autoconf.h>
#include <wanted-image-verify.h>
#include <wanted_log.h>

#ifndef CONFIG_WANTED_IMAGE_SIGNING_KEYS
#define CONFIG_WANTED_IMAGE_SIGNING_KEYS ""
#endif

/* Keys the firmware carries, matching the identity store's key count so one
 * rotation window covers both. */
#define IMAGE_KEYRING_SLOTS 4

/* The signed message: a length-prefixed name and version, the layer count and
 * the layer digests base-first. */
#define SIGNED_MESSAGE_MAX                                                     \
    (1 + WAPP_MAX_NAME_LEN + 1 + WAPP_MAX_VERSION_LEN + 1 +                    \
     (REGISTRY_META_MAX_LAYERS * REGISTRY_META_DIGEST_LEN))

typedef struct {
    uint32_t id;
    uint8_t key[PLATFORM_ED25519_KEY_LEN];
} image_key_t;

static image_key_t g_keyring[IMAGE_KEYRING_SLOTS];
static size_t g_keyCount;
static bool g_keyringParsed;
static bool g_runtimeEnforce;

static int hexNibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse "<id>:<64 hex>" into `out`, or false where either half is malformed. A
 * malformed entry drops the key: a key nobody can name verifies nothing. */
static bool parseKey(const char *entry, size_t len, image_key_t *out) {
    size_t i = 0;
    uint32_t id = 0;

    for (; i < len && entry[i] != ':'; i++) {
        if (entry[i] < '0' || entry[i] > '9')
            return false;
        id = id * 10 + (uint32_t)(entry[i] - '0');
    }
    if (i == 0 || i >= len)
        return false;
    i++; /* the separator */
    if (len - i != (size_t)(PLATFORM_ED25519_KEY_LEN * 2))
        return false;

    for (size_t b = 0; b < PLATFORM_ED25519_KEY_LEN; b++) {
        int hi = hexNibble(entry[i + (b * 2)]);
        int lo = hexNibble(entry[i + (b * 2) + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out->key[b] = (uint8_t)((hi << 4) | lo);
    }
    out->id = id;
    return true;
}

/* The keyring is a comma-separated "<id>:<64 hex>" list compiled into the
 * firmware, so secure boot covers it. */
static void parseKeyring(void) {
    const char *spec = CONFIG_WANTED_IMAGE_SIGNING_KEYS;
    const char *p = spec;

    g_keyringParsed = true;
    while (*p != '\0' && g_keyCount < IMAGE_KEYRING_SLOTS) {
        const char *comma = strchr(p, ',');
        size_t len = comma != NULL ? (size_t)(comma - p) : strlen(p);

        if (len > 0 && parseKey(p, len, &g_keyring[g_keyCount]))
            g_keyCount++;
        else if (len > 0)
            LOG_ERROR("image keyring: entry %zu is malformed", g_keyCount);

        if (comma == NULL)
            break;
        p = comma + 1;
    }
}

static const uint8_t *keyById(uint32_t id) {
    if (!g_keyringParsed)
        parseKeyring();
    for (size_t i = 0; i < g_keyCount; i++) {
        if (g_keyring[i].id == id)
            return g_keyring[i].key;
    }
    return NULL;
}

size_t WantedImageSignedMessage(const reg_entry_t *entry,
                                const registry_meta_t *meta, uint8_t *out,
                                size_t outLen) {
    size_t nameLen = strnlen(entry->name, WAPP_MAX_NAME_LEN);
    size_t verLen = strnlen(entry->version, WAPP_MAX_VERSION_LEN);
    size_t need = 1 + nameLen + 1 + verLen + 1 +
                  ((size_t)meta->layerCount * REGISTRY_META_DIGEST_LEN);
    size_t n = 0;

    if (nameLen == 0 || verLen == 0 || meta->layerCount == 0 || need > outLen)
        return 0;

    out[n++] = (uint8_t)nameLen;
    memcpy(out + n, entry->name, nameLen);
    n += nameLen;
    out[n++] = (uint8_t)verLen;
    memcpy(out + n, entry->version, verLen);
    n += verLen;
    out[n++] = meta->layerCount;
    for (uint8_t i = 0; i < meta->layerCount; i++) {
        memcpy(out + n, meta->layerDigest[i], REGISTRY_META_DIGEST_LEN);
        n += REGISTRY_META_DIGEST_LEN;
    }
    return n;
}

/* Hash one mapped layer into `out`. */
static bool hashLayer(const uint8_t *bytes, size_t len,
                      uint8_t out[REGISTRY_META_DIGEST_LEN]) {
    void *ctx = PlatformSha256New();
    if (ctx == NULL)
        return false;
    PlatformSha256Update(ctx, bytes, len);
    PlatformSha256Final(ctx, out);
    PlatformSha256Free(ctx);
    return true;
}

/* Compare every mapped layer against the record. `wapp_t.layers[]` runs
 * topmost-first and the record runs base-first, so the indexes mirror. */
static bool digestsMatch(const registry_meta_t *meta, const wapp_t *w) {
    uint8_t digest[REGISTRY_META_DIGEST_LEN];

    if (w->layer_cnt == 0 || w->layer_cnt != meta->layerCount)
        return false;
    for (size_t i = 0; i < w->layer_cnt; i++) {
        size_t base = w->layer_cnt - 1 - i;
        if (!hashLayer(w->layers[i], w->layer_lens[i], digest))
            return false;
        if (memcmp(digest, meta->layerDigest[base], sizeof(digest)) != 0)
            return false;
    }
    return true;
}

image_verify_state_t WantedVerifyImage(const reg_entry_t *entry,
                                       const wapp_t *w) {
    registry_meta_t meta;
    uint8_t message[SIGNED_MESSAGE_MAX];
    const uint8_t *pubkey;
    size_t msgLen;

    if (entry == NULL || w == NULL)
        return IMAGE_VERIFY_NO_RECORD;
    if (PlatformRegistryMetaRead(entry, &meta) < 0)
        return IMAGE_VERIFY_NO_RECORD;
    if (meta.flags & REGISTRY_META_SEEDED)
        return IMAGE_VERIFY_SEEDED;
    if (!digestsMatch(&meta, w))
        return IMAGE_VERIFY_DIGEST_MISMATCH;
    if (!(meta.flags & REGISTRY_META_SIGNED))
        return IMAGE_VERIFY_NO_SIGNATURE;

    pubkey = keyById(meta.keyId);
    if (pubkey == NULL)
        return IMAGE_VERIFY_UNKNOWN_KEY;

    msgLen = WantedImageSignedMessage(entry, &meta, message, sizeof(message));
    if (msgLen == 0)
        return IMAGE_VERIFY_BAD_SIGNATURE;

    /* -ENOSYS from a build with no backend reads as "not valid". */
    if (PlatformEd25519Verify(pubkey, meta.signature, message, msgLen) != 0)
        return IMAGE_VERIFY_BAD_SIGNATURE;
    return IMAGE_VERIFY_OK;
}

const char *WantedImageVerifyStateName(image_verify_state_t state) {
    switch (state) {
    case IMAGE_VERIFY_OK:
        return "ok";
    case IMAGE_VERIFY_SEEDED:
        return "seeded";
    case IMAGE_VERIFY_NO_RECORD:
        return "no_record";
    case IMAGE_VERIFY_DIGEST_MISMATCH:
        return "digest_mismatch";
    case IMAGE_VERIFY_NO_SIGNATURE:
        return "no_signature";
    case IMAGE_VERIFY_UNKNOWN_KEY:
        return "unknown_key";
    case IMAGE_VERIFY_BAD_SIGNATURE:
        return "bad_signature";
    }
    return "unknown";
}

int WantedImageVerifyErrno(image_verify_state_t state) {
    switch (state) {
    case IMAGE_VERIFY_OK:
    case IMAGE_VERIFY_SEEDED:
        return 0;
    case IMAGE_VERIFY_NO_RECORD:
        return -ENOENT;
    case IMAGE_VERIFY_DIGEST_MISMATCH:
        return -EBADMSG;
    case IMAGE_VERIFY_NO_SIGNATURE:
        return -ENOMSG;
    case IMAGE_VERIFY_UNKNOWN_KEY:
        return -EACCES;
    case IMAGE_VERIFY_BAD_SIGNATURE:
        return -EILSEQ;
    }
    return -EILSEQ;
}

bool WantedImageVerifyFloor(void) {
#ifdef CONFIG_WANTED_WAPP_IMAGE_VERIFY_ENFORCE
    return true;
#else
    return false;
#endif
}

bool WantedImageVerifyEnforced(void) {
    return WantedImageVerifyFloor() || g_runtimeEnforce;
}

void WantedImageVerifyRaise(bool on) {
    if (on)
        g_runtimeEnforce = true;
}

int WantedImageVerifyGate(const reg_entry_t *entry, const wapp_t *w) {
    image_verify_state_t state = WantedVerifyImage(entry, w);
    int err = WantedImageVerifyErrno(state);

    if (err == 0)
        return 0;
    if (!WantedImageVerifyEnforced()) {
        LOG_ERROR("%s:%s image verification: %s (not enforced)", entry->name,
                  entry->version, WantedImageVerifyStateName(state));
        return 0;
    }
    LOG_ERROR("%s:%s refused: image verification %s", entry->name,
              entry->version, WantedImageVerifyStateName(state));
    return err;
}
