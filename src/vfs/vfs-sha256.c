/* SPDX-License-Identifier: Apache-2.0 */

/* /dev/sha256 — streaming SHA-256 digest device.
 *
 * Each open starts a fresh digest stream: writes feed message bytes, the
 * first read finalizes the digest and reads return it as 64 lowercase hex
 * characters. Once a read has finalized the stream, further writes are
 * rejected; close releases the stream. A wapp offloads hashing here instead
 * of carrying the SHA-256 code, constant table, and working state in its own
 * linear memory. The digest itself is computed by the PlatformSha256*
 * seam (platform/include/platform.h), so this driver never touches the
 * algorithm directly and picks up a hardware-accelerated backend for free
 * on a target that has one.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

static const char id[] = {'S', 'h', 'a', '2'};

/* Concurrent digest streams per mount (one mount per wapp). */
#define SHA256_MAX_STREAMS 2

#define SHA256_HEX_LEN ((size_t)PLATFORM_SHA256_DIGEST_LEN * 2)

/* --- driver --------------------------------------------------------------- */

typedef struct {
    bool in_use;
    bool finalized;
    void *hash; /* PlatformSha256New() handle */
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
        for (int i = 0; i < SHA256_MAX_STREAMS; i++)
            PlatformSha256Free(d->ctx->streams[i].hash);
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
            void *hash = PlatformSha256New();
            if (hash == NULL)
                return -ENOMEM;
            memset(&d->streams[i], 0, sizeof(d->streams[i]));
            d->streams[i].in_use = true;
            d->streams[i].hash = hash;
            return i;
        }
    }
    return -EBUSY;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    sha256_stream_t *s = stream(d, fd);
    if (s == NULL)
        return -EBADF;
    PlatformSha256Free(s->hash);
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

    PlatformSha256Update(s->hash, buf, nbyte);
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
        uint8_t digest[PLATFORM_SHA256_DIGEST_LEN];
        PlatformSha256Final(s->hash, digest);
        for (size_t i = 0; i < PLATFORM_SHA256_DIGEST_LEN; i++) {
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
