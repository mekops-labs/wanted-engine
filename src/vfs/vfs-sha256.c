/* SPDX-License-Identifier: Apache-2.0 */

/* /dev/sha256 — streaming SHA-256 digest device.
 *
 * Each open starts a fresh digest stream: writes feed message bytes, the
 * first read finalizes the digest and reads return it as 64 lowercase hex
 * characters. Once a read has finalized the stream, further writes are
 * rejected; close releases the stream. A wapp offloads hashing here instead
 * of carrying the SHA-256 code, constant table, and working state in its own
 * linear memory.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

static const char id[] = {'S', 'h', 'a', '2'};

/* Concurrent digest streams per mount (one mount per wapp). */
#define SHA256_MAX_STREAMS 2

#define SHA256_BLOCK_LEN 64
#define SHA256_DIGEST_LEN 32
#define SHA256_HEX_LEN ((size_t)SHA256_DIGEST_LEN * 2)

/* --- SHA-256 core (FIPS 180-4) ------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t block[SHA256_BLOCK_LEN];
    size_t blocklen;
} sha256_t;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32U - n));
}

static void sha256Init(sha256_t *s) {
    s->state[0] = 0x6a09e667;
    s->state[1] = 0xbb67ae85;
    s->state[2] = 0x3c6ef372;
    s->state[3] = 0xa54ff53a;
    s->state[4] = 0x510e527f;
    s->state[5] = 0x9b05688c;
    s->state[6] = 0x1f83d9ab;
    s->state[7] = 0x5be0cd19;
    s->bitlen = 0;
    s->blocklen = 0;
}

static void sha256Compress(sha256_t *s, const uint8_t *block) {
    uint32_t w[64];
    uint32_t v[8];

    for (size_t i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[(i * 4) + 1] << 16) |
               ((uint32_t)block[(i * 4) + 2] << 8) |
               (uint32_t)block[(i * 4) + 3];
    }
    for (size_t i = 16; i < 64; i++) {
        uint32_t s0 =
            rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 =
            rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    memcpy(v, s->state, sizeof(v));

    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25);
        uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
        uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22);
        uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
        uint32_t t2 = s0 + maj;

        v[7] = v[6];
        v[6] = v[5];
        v[5] = v[4];
        v[4] = v[3] + t1;
        v[3] = v[2];
        v[2] = v[1];
        v[1] = v[0];
        v[0] = t1 + t2;
    }

    for (int i = 0; i < 8; i++)
        s->state[i] += v[i];
}

static void sha256Update(sha256_t *s, const uint8_t *data, size_t len) {
    s->bitlen += (uint64_t)len * 8U;
    while (len > 0) {
        size_t take = SHA256_BLOCK_LEN - s->blocklen;
        if (take > len)
            take = len;
        memcpy(s->block + s->blocklen, data, take);
        s->blocklen += take;
        data += take;
        len -= take;
        if (s->blocklen == SHA256_BLOCK_LEN) {
            sha256Compress(s, s->block);
            s->blocklen = 0;
        }
    }
}

static void sha256Final(sha256_t *s, uint8_t out[SHA256_DIGEST_LEN]) {
    uint64_t bitlen = s->bitlen;

    s->block[s->blocklen++] = 0x80;
    if (s->blocklen > SHA256_BLOCK_LEN - 8) {
        memset(s->block + s->blocklen, 0, SHA256_BLOCK_LEN - s->blocklen);
        sha256Compress(s, s->block);
        s->blocklen = 0;
    }
    memset(s->block + s->blocklen, 0, SHA256_BLOCK_LEN - 8 - s->blocklen);
    for (int i = 0; i < 8; i++)
        s->block[SHA256_BLOCK_LEN - 1 - i] = (uint8_t)(bitlen >> (8 * i));
    sha256Compress(s, s->block);

    for (size_t i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(s->state[i] >> 24);
        out[(i * 4) + 1] = (uint8_t)(s->state[i] >> 16);
        out[(i * 4) + 2] = (uint8_t)(s->state[i] >> 8);
        out[(i * 4) + 3] = (uint8_t)(s->state[i]);
    }
}

/* --- driver --------------------------------------------------------------- */

typedef struct {
    bool in_use;
    bool finalized;
    sha256_t hash;
    char hex[SHA256_HEX_LEN];
    size_t read_off;
} sha256_stream_t;

struct vfs_driver_ctx_t {
    sha256_stream_t streams[SHA256_MAX_STREAMS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsSha256Init(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;

    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (drv == NULL)
        return NULL;
    struct vfs_driver_ctx_t *ctx = WantedMalloc(sizeof(*ctx));
    if (ctx == NULL) {
        WantedFree(drv);
        return NULL;
    }
    memset(drv, 0, sizeof(*drv));
    memset(ctx, 0, sizeof(*ctx));

    drv->bytesId = *(const uint32_t *)(id);
    drv->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    drv->ctx = ctx;
    drv->Destroy = _Destroy;
    drv->Open = _Open;
    drv->Close = _Close;
    drv->Stat = _Stat;
    drv->Read = _Read;
    drv->Write = _Write;

    return drv;
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d->ctx != NULL) {
        memset(d->ctx, 0, sizeof(*d->ctx));
        WantedFree(d->ctx);
    }
    memset(d, 0, sizeof(*d));
    WantedFree(d);
    return 0;
}

static sha256_stream_t *stream(vfs_driver_ctx_t d, int fd) {
    if (d == NULL || fd < 0 || fd >= SHA256_MAX_STREAMS)
        return NULL;
    if (!d->streams[fd].in_use)
        return NULL;
    return &d->streams[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    if (d == NULL)
        return -EINVAL;
    /* The device is a single node: no sub-paths exist beneath it. */
    if (path != NULL && path[0] != '\0' && !(path[0] == '/' && path[1] == '\0'))
        return -ENOENT;

    for (int i = 0; i < SHA256_MAX_STREAMS; i++) {
        if (!d->streams[i].in_use) {
            memset(&d->streams[i], 0, sizeof(d->streams[i]));
            d->streams[i].in_use = true;
            sha256Init(&d->streams[i].hash);
            return i;
        }
    }
    return -EBUSY;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    sha256_stream_t *s = stream(d, fd);
    if (s == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    const sha256_stream_t *s = stream(d, fd);
    if (s == NULL)
        return -EBADF;
    if (stat == NULL)
        return -EINVAL;

    memset(stat, 0, sizeof(*stat));
    stat->dev = *(const uint32_t *)(id);
    stat->ino = (uint32_t)fd;
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    stat->size = SHA256_HEX_LEN;
    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    sha256_stream_t *s = stream(d, fd);
    if (s == NULL)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;
    /* The digest is sealed once read; a fresh open starts a new stream. */
    if (s->finalized)
        return -EINVAL;

    sha256Update(&s->hash, buf, nbyte);
    return (int)nbyte;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    static const char hexdigits[] = "0123456789abcdef";
    sha256_stream_t *s = stream(d, fd);

    if (s == NULL)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;

    if (!s->finalized) {
        uint8_t digest[SHA256_DIGEST_LEN];
        sha256Final(&s->hash, digest);
        for (size_t i = 0; i < SHA256_DIGEST_LEN; i++) {
            s->hex[i * 2] = hexdigits[digest[i] >> 4];
            s->hex[(i * 2) + 1] = hexdigits[digest[i] & 0x0f];
        }
        s->finalized = true;
        s->read_off = 0;
    }

    size_t left = SHA256_HEX_LEN - s->read_off;
    if (left == 0)
        return 0;
    if (nbyte > left)
        nbyte = left;
    memcpy(buf, s->hex + s->read_off, nbyte);
    s->read_off += nbyte;
    return (int)nbyte;
}
