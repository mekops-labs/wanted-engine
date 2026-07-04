/* SPDX-License-Identifier: Apache-2.0 */

/* /dev/inflate — streaming gzip decompression device.
 *
 * Each open decompresses one gzip member. The write stream is length-prefixed:
 * the first 4 bytes declare the compressed member size (LE u32, header and
 * trailer included), then the member bytes follow in any chunking. Reads drain
 * the decompressed output; a short write means the output buffer is full —
 * read before writing more. Reading an empty stream mid-member returns
 * -EAGAIN; a finished, drained stream reads 0. The trailer CRC32 and ISIZE are
 * validated; any malformed input fails the stream with -EIO until close.
 *
 * The length prefix exists because the decoder cannot resume after running
 * out of *input* mid-symbol (output pauses are fine). Knowing where the member
 * ends lets the driver decode eagerly with a safe input margin and finish
 * deterministically on the declared last byte. The margin makes the worst-case
 * input per decoded byte explicit; a stream that exceeds it (a run of empty
 * deflate blocks — nothing a whole-file compressor emits) fails as malformed.
 *
 * A wapp offloads decompression here instead of carrying the inflate code and
 * its 32 KiB history window in its own linear memory; the window lives in
 * engine memory for the lifetime of the open.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

#include <uzlib.h>

static const char id[] = {'I', 'n', 'f', 'l'};

/* Length prefix declaring the compressed member size. */
#define INFLATE_PREFIX_LEN 4
/* DEFLATE history window — back-references reach up to 32 KiB behind. */
#define INFLATE_DICT_LEN 32768U
/* Compressed-input staging buffer. */
#define INFLATE_CARRY_LEN 4096U
/* Decompressed-output buffer drained by reads. */
#define INFLATE_OUT_LEN 4096U
/* Input the decoder may need beyond the literal bytes it produces: one
 * dynamic-Huffman block header plus one worst-case symbol. */
#define INFLATE_IN_MARGIN 350U
/* Worst-case compressed bytes consumed per decompressed byte (a run of
 * 1-byte stored blocks: ~5 bytes framing + 1 data byte). */
#define INFLATE_IN_PER_OUT 6U

/* gzip member framing (RFC 1952). */
#define GZIP_FIXED_HDR_LEN 10U
#define GZIP_TRAILER_LEN 8U
#define GZIP_ID1 0x1fU
#define GZIP_ID2 0x8bU
#define GZIP_CM_DEFLATE 8U
#define GZIP_FLG_FHCRC 0x02U
#define GZIP_FLG_FEXTRA 0x04U
#define GZIP_FLG_FNAME 0x08U
#define GZIP_FLG_FCOMMENT 0x10U
#define GZIP_FLG_RESERVED 0xe0U

typedef enum {
    ST_PREFIX,      /* collecting the 4-byte length prefix */
    ST_HDR_FIXED,   /* collecting the 10-byte fixed gzip header */
    ST_HDR_XLEN,    /* collecting the 2-byte FEXTRA length */
    ST_HDR_EXTRA,   /* skipping the FEXTRA payload */
    ST_HDR_NAME,    /* skipping the NUL-terminated FNAME */
    ST_HDR_COMMENT, /* skipping the NUL-terminated FCOMMENT */
    ST_HDR_HCRC,    /* skipping the 2-byte header CRC */
    ST_BODY,        /* DEFLATE stream through the decoder */
    ST_TRAILER,     /* collecting the 8-byte CRC32+ISIZE trailer */
    ST_DONE,        /* member decoded and validated */
    ST_ERROR,       /* malformed input; sticky until close */
} inflate_state_t;

typedef struct {
    inflate_state_t state;
    struct uzlib_uncomp uz;
    uint8_t hdr_flg;
    uint32_t declared_len; /* member size from the prefix */
    uint32_t received;     /* member bytes accepted so far */
    uint32_t skip_left;    /* remaining FEXTRA/HCRC bytes to skip */
    uint32_t crc;          /* running CRC32 over decompressed output */
    uint32_t out_total;    /* total decompressed bytes (ISIZE check) */
    size_t collect_len;    /* bytes gathered in collect[] for the current
                              fixed-size element (prefix/header/trailer) */
    uint8_t collect[GZIP_FIXED_HDR_LEN];
    size_t carry_pos;
    size_t carry_len;
    size_t out_pos;
    size_t out_len;
    uint8_t carry[INFLATE_CARRY_LEN];
    uint8_t out[INFLATE_OUT_LEN];
    uint8_t dict[INFLATE_DICT_LEN];
} inflate_stream_t;

struct vfs_driver_ctx_t {
    inflate_stream_t *s; /* NULL when no stream is open */
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsInflateInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;

    uzlib_init();

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
        if (d->ctx->s != NULL)
            WantedFree(d->ctx->s);
        memset(d->ctx, 0, sizeof(*d->ctx));
        WantedFree(d->ctx);
    }
    memset(d, 0, sizeof(*d));
    WantedFree(d);
    return 0;
}

static uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
           ((uint32_t)b[3] << 24);
}

static bool inputComplete(const inflate_stream_t *s) {
    return s->state > ST_PREFIX && s->received == s->declared_len;
}

/* Pull one byte from the carry buffer; false when it is empty. */
static bool carryByte(inflate_stream_t *s, uint8_t *b) {
    if (s->carry_pos >= s->carry_len)
        return false;
    *b = s->carry[s->carry_pos++];
    return true;
}

/* Advance the header/trailer state machine and the decoder as far as the
 * carried input and output space allow. Never starves the decoder: mid-member
 * it caps each decode step so the worst-case input need is covered by what is
 * already carried; once the declared last byte arrived it decodes freely (a
 * starve then means a truncated member — malformed). Sets ST_ERROR on any
 * malformed element. */
static void pump(inflate_stream_t *s) {
    uint8_t b;

    for (;;) {
        switch (s->state) {
        case ST_HDR_FIXED:
            while (s->collect_len < GZIP_FIXED_HDR_LEN && carryByte(s, &b))
                s->collect[s->collect_len++] = b;
            if (s->collect_len < GZIP_FIXED_HDR_LEN)
                return;
            if (s->collect[0] != GZIP_ID1 || s->collect[1] != GZIP_ID2 ||
                s->collect[2] != GZIP_CM_DEFLATE ||
                (s->collect[3] & GZIP_FLG_RESERVED) != 0) {
                s->state = ST_ERROR;
                return;
            }
            s->hdr_flg = s->collect[3];
            s->collect_len = 0;
            s->state = ST_HDR_XLEN;
            break;

        case ST_HDR_XLEN:
            if ((s->hdr_flg & GZIP_FLG_FEXTRA) == 0) {
                s->state = ST_HDR_NAME;
                break;
            }
            while (s->collect_len < 2 && carryByte(s, &b))
                s->collect[s->collect_len++] = b;
            if (s->collect_len < 2)
                return;
            s->skip_left =
                (uint32_t)s->collect[0] | ((uint32_t)s->collect[1] << 8);
            s->collect_len = 0;
            s->state = ST_HDR_EXTRA;
            break;

        case ST_HDR_EXTRA:
            while (s->skip_left > 0 && carryByte(s, &b))
                s->skip_left--;
            if (s->skip_left > 0)
                return;
            s->state = ST_HDR_NAME;
            break;

        case ST_HDR_NAME:
            if ((s->hdr_flg & GZIP_FLG_FNAME) != 0) {
                do {
                    if (!carryByte(s, &b))
                        return;
                } while (b != 0);
            }
            s->state = ST_HDR_COMMENT;
            break;

        case ST_HDR_COMMENT:
            if ((s->hdr_flg & GZIP_FLG_FCOMMENT) != 0) {
                do {
                    if (!carryByte(s, &b))
                        return;
                } while (b != 0);
            }
            s->skip_left = ((s->hdr_flg & GZIP_FLG_FHCRC) != 0) ? 2 : 0;
            s->state = ST_HDR_HCRC;
            break;

        case ST_HDR_HCRC:
            while (s->skip_left > 0 && carryByte(s, &b))
                s->skip_left--;
            if (s->skip_left > 0)
                return;
            uzlib_uncompress_init(&s->uz, s->dict, INFLATE_DICT_LEN);
            s->crc = ~0U;
            s->state = ST_BODY;
            break;

        case ST_BODY: {
            size_t avail = s->carry_len - s->carry_pos;
            size_t space = INFLATE_OUT_LEN - s->out_len;
            size_t budget = space;

            if (space == 0)
                return;
            if (!inputComplete(s)) {
                if (avail <= INFLATE_IN_MARGIN)
                    return;
                size_t safe = (avail - INFLATE_IN_MARGIN) / INFLATE_IN_PER_OUT;
                if (safe == 0)
                    return;
                if (budget > safe)
                    budget = safe;
            }

            s->uz.source = s->carry + s->carry_pos;
            s->uz.source_limit = s->carry + s->carry_len;
            s->uz.source_read_cb = NULL;
            s->uz.dest_start = s->out + s->out_len;
            s->uz.dest = s->out + s->out_len;
            s->uz.dest_limit = s->out + s->out_len + budget;

            int res = uzlib_uncompress(&s->uz);

            size_t consumed =
                (size_t)(s->uz.source - (s->carry + s->carry_pos));
            size_t produced = (size_t)(s->uz.dest - (s->out + s->out_len));
            s->carry_pos += consumed;
            if (produced > 0) {
                s->crc = uzlib_crc32(s->out + s->out_len,
                                     (unsigned int)produced, s->crc);
                s->out_total += (uint32_t)produced;
                s->out_len += produced;
            }

            if (res == TINF_DONE) {
                s->collect_len = 0;
                s->state = ST_TRAILER;
            } else if (res != TINF_OK) {
                s->state = ST_ERROR;
                return;
            }
            break;
        }

        case ST_TRAILER:
            while (s->collect_len < GZIP_TRAILER_LEN && carryByte(s, &b))
                s->collect[s->collect_len++] = b;
            if (s->collect_len < GZIP_TRAILER_LEN)
                return;
            if (le32(s->collect) != ~s->crc ||
                le32(s->collect + 4) != s->out_total ||
                s->carry_pos != s->carry_len || !inputComplete(s)) {
                s->state = ST_ERROR;
                return;
            }
            s->state = ST_DONE;
            return;

        default:
            return;
        }
    }
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    if (d == NULL)
        return -EINVAL;
    /* The device is a single node: no sub-paths exist beneath it. */
    if (path != NULL && path[0] != '\0' && !(path[0] == '/' && path[1] == '\0'))
        return -ENOENT;
    /* One member decode at a time per mount. */
    if (d->s != NULL)
        return -EBUSY;

    d->s = WantedMalloc(sizeof(*d->s));
    if (d->s == NULL)
        return -ENOMEM;
    memset(d->s, 0, sizeof(*d->s));
    d->s->state = ST_PREFIX;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)fd;
    if (d == NULL || d->s == NULL)
        return -EBADF;
    WantedFree(d->s);
    d->s = NULL;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)fd;
    if (d == NULL || d->s == NULL)
        return -EBADF;
    if (stat == NULL)
        return -EINVAL;

    memset(stat, 0, sizeof(*stat));
    stat->dev = *(const uint32_t *)(id);
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    stat->size = (uint32_t)(d->s->out_len - d->s->out_pos);
    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)fd;
    inflate_stream_t *s = (d != NULL) ? d->s : NULL;
    const uint8_t *in = buf;
    size_t accepted = 0;

    if (s == NULL)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;
    if (s->state == ST_ERROR)
        return -EIO;

    /* Length prefix: the member size must be declared before any member
     * byte. */
    while (s->state == ST_PREFIX && accepted < nbyte) {
        s->collect[s->collect_len++] = in[accepted++];
        if (s->collect_len == INFLATE_PREFIX_LEN) {
            s->declared_len = le32(s->collect);
            s->collect_len = 0;
            if (s->declared_len < GZIP_FIXED_HDR_LEN + GZIP_TRAILER_LEN) {
                s->state = ST_ERROR;
                return -EINVAL;
            }
            s->state = ST_HDR_FIXED;
        }
    }

    while (accepted < nbyte) {
        if (s->received == s->declared_len)
            return (accepted > 0) ? (int)accepted : -EFBIG;

        /* Compact the carry so appended input is contiguous. */
        if (s->carry_pos > 0) {
            memmove(s->carry, s->carry + s->carry_pos,
                    s->carry_len - s->carry_pos);
            s->carry_len -= s->carry_pos;
            s->carry_pos = 0;
        }

        size_t space = INFLATE_CARRY_LEN - s->carry_len;
        size_t take = nbyte - accepted;
        if (take > space)
            take = space;
        if (take > s->declared_len - s->received)
            take = s->declared_len - s->received;

        if (take == 0) {
            /* Carry is full: only draining output frees it. */
            pump(s);
            if (s->state == ST_ERROR)
                return (accepted > 0) ? (int)accepted : -EIO;
            if (s->carry_len - s->carry_pos == INFLATE_CARRY_LEN)
                return (accepted > 0) ? (int)accepted : -EAGAIN;
            continue;
        }

        memcpy(s->carry + s->carry_len, in + accepted, take);
        s->carry_len += take;
        s->received += (uint32_t)take;
        accepted += take;

        pump(s);
        if (s->state == ST_ERROR)
            return (accepted > 0) ? (int)accepted : -EIO;
    }

    return (int)accepted;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)fd;
    inflate_stream_t *s = (d != NULL) ? d->s : NULL;

    if (s == NULL)
        return -EBADF;
    if (buf == NULL)
        return -EINVAL;
    if (s->state == ST_ERROR)
        return -EIO;

    if (s->out_pos == s->out_len) {
        /* Drained: recycle the output buffer and pull more through. */
        s->out_pos = 0;
        s->out_len = 0;
        pump(s);
        if (s->state == ST_ERROR)
            return -EIO;
        if (s->out_len == 0)
            return (s->state == ST_DONE) ? 0 : -EAGAIN;
    }

    size_t left = s->out_len - s->out_pos;
    if (nbyte > left)
        nbyte = left;
    memcpy(buf, s->out + s->out_pos, nbyte);
    s->out_pos += nbyte;
    return (int)nbyte;
}
