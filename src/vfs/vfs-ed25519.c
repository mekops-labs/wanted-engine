/* SPDX-License-Identifier: Apache-2.0 */

/* /dev/ed25519 — Ed25519 signature-verification device.
 *
 * Each open starts one verification. The write stream is framed: the first
 * 32 bytes are the raw public key, the next 64 bytes the signature, and every
 * byte after that is the message. Reading returns the verdict as a text
 * token — "ok" when the signature verifies, "fail" when it does not — and
 * seals the stream; a fresh open starts the next verification. The engine
 * holds no keys: the caller supplies the public key it trusts, so key custody
 * stays with the wapp and the engine only performs the curve arithmetic
 * (hardware-accelerated where the platform provides it).
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

static const char id[] = {'E', 'd', '2', '5'};

/* Longest message a single verification accepts. The message is buffered in
 * engine memory until the verdict read, so this bounds the per-stream cost. */
#define ED25519_MAX_MSG_LEN ((size_t)64 * 1024)

#define ED25519_HDR_LEN (PLATFORM_ED25519_KEY_LEN + PLATFORM_ED25519_SIG_LEN)

#define ED25519_VERDICT_OK "ok"
#define ED25519_VERDICT_FAIL "fail"
/* Longest verdict token (excluding NUL). */
#define ED25519_VERDICT_MAX 4

struct vfs_driver_ctx_t {
    bool in_use;
    bool sealed; /* verdict computed; stream accepts no more writes */
    uint8_t hdr[ED25519_HDR_LEN];
    uint8_t *msg;
    size_t written; /* total bytes accepted, header included */
    char verdict[ED25519_VERDICT_MAX];
    size_t verdict_len;
    size_t read_off;
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsEd25519Init(const wapp_t *wapp, const char *options) {
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

static void reset(vfs_driver_ctx_t d) {
    if (d->msg != NULL) {
        WantedFree(d->msg);
        d->msg = NULL;
    }
    memset(d, 0, sizeof(*d));
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d->ctx != NULL) {
        reset(d->ctx);
        WantedFree(d->ctx);
    }
    memset(d, 0, sizeof(*d));
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    if (d == NULL)
        return -EINVAL;
    /* The device is a single node: no sub-paths exist beneath it. */
    if (path != NULL && path[0] != '\0' && !(path[0] == '/' && path[1] == '\0'))
        return -ENOENT;
    /* One verification at a time per mount. */
    if (d->in_use)
        return -EBUSY;

    reset(d);
    d->msg = WantedMalloc(ED25519_MAX_MSG_LEN);
    if (d->msg == NULL)
        return -ENOMEM;
    d->in_use = true;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)fd;
    if (d == NULL || !d->in_use)
        return -EBADF;
    reset(d);
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)fd;
    if (d == NULL || !d->in_use)
        return -EBADF;
    if (stat == NULL)
        return -EINVAL;

    memset(stat, 0, sizeof(*stat));
    stat->dev = *(const uint32_t *)(id);
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    stat->size = (uint32_t)d->verdict_len;
    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)fd;
    const uint8_t *in = buf;
    size_t left = nbyte;

    if (d == NULL || !d->in_use)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;
    /* The verdict is computed once; a fresh open starts a new verification. */
    if (d->sealed)
        return -EINVAL;

    if (d->written < ED25519_HDR_LEN) {
        size_t take = ED25519_HDR_LEN - d->written;
        if (take > left)
            take = left;
        memcpy(d->hdr + d->written, in, take);
        d->written += take;
        in += take;
        left -= take;
    }

    if (left > 0) {
        size_t msg_len = d->written - ED25519_HDR_LEN;
        if (left > ED25519_MAX_MSG_LEN - msg_len)
            return -EFBIG;
        memcpy(d->msg + msg_len, in, left);
        d->written += left;
    }

    return (int)nbyte;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)fd;

    if (d == NULL || !d->in_use)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;
    /* A verdict needs at least the full key + signature header. */
    if (d->written < ED25519_HDR_LEN)
        return -EINVAL;

    if (!d->sealed) {
        int r = PlatformEd25519Verify(d->hdr, d->hdr + PLATFORM_ED25519_KEY_LEN,
                                      d->msg, d->written - ED25519_HDR_LEN);
        if (r != 0 && r != -EBADMSG)
            return r;
        const char *v = (r == 0) ? ED25519_VERDICT_OK : ED25519_VERDICT_FAIL;
        d->verdict_len = strlen(v);
        memcpy(d->verdict, v, d->verdict_len);
        d->sealed = true;
        d->read_off = 0;
    }

    size_t left = d->verdict_len - d->read_off;
    if (left == 0)
        return 0;
    if (nbyte > left)
        nbyte = left;
    memcpy(buf, d->verdict + d->read_off, nbyte);
    d->read_off += nbyte;
    return (int)nbyte;
}
