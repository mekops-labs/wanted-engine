#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-pipe.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Named pipe DevFS sub-driver for /dev/pipe/<name>.
 *
 * Each named pipe is a fixed-size ring buffer. Write creates the pipe if
 * absent; read creates it too (idempotent). A pipe is destroyed when both
 * the last reader and last writer close. The driver itself is a directory
 * driver registered under the name "pipe" in DevFS; DevFS prefix-matches
 * "/dev/pipe/t" → sub_path "t" and calls PipeDriver_Open("t", flags). */

#define PIPE_BUF_SIZE  4096
#define PIPE_MAX_PIPES 8

typedef struct named_pipe_t {
    char    name[MAX_ENTRY_NAME_LEN];
    uint8_t buf[PIPE_BUF_SIZE];
    size_t  rpos;
    size_t  data_len;
    int     writers;
    int     readers;
    bool    active;
} named_pipe_t;

typedef struct pipe_ctx_t {
    named_pipe_t pipes[PIPE_MAX_PIPES];
} pipe_ctx_t;

typedef struct pipe_handle_t {
    named_pipe_t *pipe; /* NULL for root directory handle */
    int           flags;
    bool          is_root;
} pipe_handle_t;

/* ── Driver operations ─────────────────────────────────────────────────────── */

static named_pipe_t *FindPipe(pipe_ctx_t *ctx, const char *name) {
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (ctx->pipes[i].active &&
            strncmp(ctx->pipes[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return &ctx->pipes[i];
    }
    return NULL;
}

static named_pipe_t *AllocPipe(pipe_ctx_t *ctx, const char *name) {
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (!ctx->pipes[i].active) {
            named_pipe_t *p = &ctx->pipes[i];
            memset(p, 0, sizeof(*p));
            strncpy(p->name, name, MAX_ENTRY_NAME_LEN - 1);
            p->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
            p->active = true;
            return p;
        }
    }
    return NULL;
}

static int _pipe_Open(vfs_driver_ctx_t dctx, const char *path,
                      vfs_oflags_t flags) {
    /* This is never called; PipeDriverCreate returns a driver whose Open
     * is called via the wrapper below.  Satisfy the interface. */
    (void)dctx; (void)path; (void)flags;
    return 0;
}

static int _pipe_Close(vfs_driver_ctx_t dctx, int fd) {
    (void)dctx; (void)fd;
    return 0;
}

static int _pipe_Destroy(vfs_driver_t *drv) {
    if (drv && drv->ctx)
        WantedFree(drv->ctx);
    if (drv)
        WantedFree(drv);
    return 0;
}

/* Template used by bridge helpers to forward calls into the typed API. */
static void BuildTmpDriver(vfs_driver_t *drv, vfs_driver_ctx_t ctx) {
    memset(drv, 0, sizeof(*drv));
    drv->bytesId  = 0x65706950;
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx      = ctx;
}

/* ── Public DevFS-sub-driver interface ─────────────────────────────────────── */

/* Called by DevFS with the suffix AFTER the "pipe" prefix, e.g. "t" or "/". */
void *PipeDriver_Open(vfs_ctx_t c, const vfs_driver_t *drv,
                      const char *suffix, vfs_oflags_t flags, int *out_err) {
    (void)c;
    pipe_ctx_t *ctx = (pipe_ctx_t *)drv->ctx;

    pipe_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        if (out_err) *out_err = -ENOMEM;
        return NULL;
    }

    /* Empty suffix or "/" → root directory open (ls /dev/pipe). */
    if (!suffix || *suffix == '\0' || strcmp(suffix, "/") == 0) {
        h->pipe    = NULL;
        h->flags   = flags;
        h->is_root = true;
        if (out_err) *out_err = 0;
        return h;
    }

    named_pipe_t *p = FindPipe(ctx, suffix);
    if (!p)
        p = AllocPipe(ctx, suffix);
    if (!p) {
        WantedFree(h);
        if (out_err) *out_err = -ENOSPC;
        return NULL;
    }

    if ((flags & VFS_O_RDWR) == VFS_O_RDWR) {
        WantedFree(h);
        if (out_err) *out_err = -EINVAL;
        return NULL;
    }

    if ((flags & 3) == VFS_O_WRONLY)
        p->writers++;
    else
        p->readers++;

    h->pipe    = p;
    h->flags   = flags;
    h->is_root = false;
    if (out_err) *out_err = 0;
    return h;
}

int PipeDriver_Close(vfs_ctx_t c, const vfs_driver_t *drv, void *handle) {
    (void)c; (void)drv;
    pipe_handle_t *h = handle;
    if (!h) return -EBADF;

    if (!h->is_root && h->pipe) {
        named_pipe_t *p = h->pipe;
        if ((h->flags & 3) == VFS_O_WRONLY)
            p->writers--;
        else
            p->readers--;
        /* Keep pipe alive while data is pending so a later reader can drain it. */
        if (p->writers <= 0 && p->readers <= 0 && p->data_len == 0)
            p->active = false;
    }
    WantedFree(h);
    return 0;
}

int PipeDriver_Read(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                    void *buf, size_t nbyte) {
    (void)c; (void)drv;
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe) return -EBADF;

    named_pipe_t *p = h->pipe;
    if (p->data_len == 0) {
        if (p->writers <= 0) {
            /* All data consumed, no writers — destroy if no other readers. */
            if (p->readers <= 1 /* this handle */ && p->writers <= 0)
                p->active = false;
            return 0;
        }
        return -EAGAIN;
    }

    size_t n = nbyte < p->data_len ? nbyte : p->data_len;
    size_t first = PIPE_BUF_SIZE - p->rpos;
    if (n <= first) {
        memcpy(buf, p->buf + p->rpos, n);
    } else {
        memcpy(buf, p->buf + p->rpos, first);
        memcpy((uint8_t *)buf + first, p->buf, n - first);
    }
    p->rpos = (p->rpos + n) % PIPE_BUF_SIZE;
    p->data_len -= n;
    return (int)n;
}

int PipeDriver_Write(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                     const void *buf, size_t nbyte) {
    (void)c; (void)drv;
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe) return -EBADF;

    named_pipe_t *p = h->pipe;
    size_t space = PIPE_BUF_SIZE - p->data_len;
    size_t n = nbyte < space ? nbyte : space;
    if (n == 0) return -EAGAIN;

    size_t wpos  = (p->rpos + p->data_len) % PIPE_BUF_SIZE;
    size_t first = PIPE_BUF_SIZE - wpos;
    if (n <= first) {
        memcpy(p->buf + wpos, buf, n);
    } else {
        memcpy(p->buf + wpos, buf, first);
        memcpy(p->buf, (const uint8_t *)buf + first, n - first);
    }
    p->data_len += n;
    return (int)n;
}

int PipeDriver_Stat(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                    vfs_stat_t *stat) {
    (void)c; (void)drv;
    pipe_handle_t *h = handle;
    if (!h) return -EBADF;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = h->is_root ? VFS_FILETYPE_DIRECTORY
                                : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

int PipeDriver_ReadDir(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                       void *buf, size_t bufLen, uint64_t *cookie,
                       size_t *bufUsed) {
    (void)c;
    pipe_handle_t *h = handle;
    if (!h || !h->is_root) return -EBADF;

    pipe_ctx_t *ctx = (pipe_ctx_t *)drv->ctx;
    vfs_dir_entry_t entries[PIPE_MAX_PIPES];
    size_t count = 0;
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (ctx->pipes[i].active) {
            entries[count].name = ctx->pipes[i].name;
            entries[count].type = VFS_FILETYPE_CHARACTER_DEVICE;
            count++;
        }
    }
    return VfsFlatDirReadDir(entries, count, buf, bufLen, cookie, bufUsed);
}

/* ── DevFS integration bridge ──────────────────────────────────────────────── */

/* The pipe driver is registered in DevFS under the name "pipe".  DevFS calls
 * the generic vfs_driver_t interface, so we need a thin wrapper that unpacks
 * the pipe-specific handle and calls the typed functions above.
 *
 * DevFS passes sub-paths like "t" (for /dev/pipe/t) or "/" (for /dev/pipe)
 * to drv->Open.  We store the pipe_ctx_t in drv->ctx and reconstruct handles
 * by allocating them ourselves — no per-call fd integer. */

static int _bridge_Open(vfs_driver_ctx_t dctx, const char *path,
                        vfs_oflags_t flags) {
    /* DevFS calls this but passes the driver's ctx directly; the result
     * (an int fd) is unused — PipeDriver_Open is called through DevFS's
     * sub-driver path.  Just return 0. */
    (void)dctx; (void)path; (void)flags;
    return 0;
}

/* The actual Open/Close/Read/Write/Stat/ReadDir path goes through DevFS which
 * calls the driver->Open returning an int fd, then the other ops with that fd.
 * Pipes need pointer-typed handles.  We smuggle the handle pointer through the
 * int fd by allocating handle on heap and casting to int — only valid on
 * platforms where sizeof(void*) <= sizeof(int) is NOT guaranteed, so instead
 * we use a flat handle table per driver. */

#define BRIDGE_MAX_HANDLES 16

typedef struct bridge_state_t {
    pipe_ctx_t      pipe_ctx;
    void           *handles[BRIDGE_MAX_HANDLES];
} bridge_state_t;

static int AllocHandle(bridge_state_t *s, void *h) {
    for (int i = 0; i < BRIDGE_MAX_HANDLES; i++) {
        if (!s->handles[i]) {
            s->handles[i] = h;
            return i;
        }
    }
    return -EMFILE;
}

static void FreeHandle(bridge_state_t *s, int fd) {
    if (fd >= 0 && fd < BRIDGE_MAX_HANDLES)
        s->handles[fd] = NULL;
}

static void *GetHandle(bridge_state_t *s, int fd) {
    if (fd < 0 || fd >= BRIDGE_MAX_HANDLES)
        return NULL;
    return s->handles[fd];
}

static int _bOpen(vfs_driver_ctx_t dctx, const char *path, vfs_oflags_t flags) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    int out_err = 0;
    /* Build a temporary vfs_driver_t pointing at our pipe_ctx to pass through
     * PipeDriver_Open's interface. */
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    void *h = PipeDriver_Open(NULL, &tmp, path, flags, &out_err);
    if (!h) return out_err;
    int fd = AllocHandle(s, h);
    if (fd < 0) {
        PipeDriver_Close(NULL, &tmp, h);
        return fd;
    }
    return fd;
}

static int _bClose(vfs_driver_ctx_t dctx, int fd) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = GetHandle(s, fd);
    if (!h) return -EBADF;
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    int r = PipeDriver_Close(NULL, &tmp, h);
    FreeHandle(s, fd);
    return r;
}

static int _bRead(vfs_driver_ctx_t dctx, int fd, void *buf, size_t nbyte) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = GetHandle(s, fd);
    if (!h) return -EBADF;
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    return PipeDriver_Read(NULL, &tmp, h, buf, nbyte);
}

static int _bWrite(vfs_driver_ctx_t dctx, int fd, const void *buf,
                   size_t nbyte) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = GetHandle(s, fd);
    if (!h) return -EBADF;
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    return PipeDriver_Write(NULL, &tmp, h, buf, nbyte);
}

static int _bStat(vfs_driver_ctx_t dctx, int fd, vfs_stat_t *stat) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = GetHandle(s, fd);
    if (!h) return -EBADF;
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    return PipeDriver_Stat(NULL, &tmp, h, stat);
}

static int _bReadDir(vfs_driver_ctx_t dctx, int fd, void *buf, size_t bufLen,
                     uint64_t *cookie, size_t *bufUsed) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = GetHandle(s, fd);
    if (!h) return -EBADF;
    vfs_driver_t tmp; BuildTmpDriver(&tmp, (vfs_driver_ctx_t)&s->pipe_ctx);
    return PipeDriver_ReadDir(NULL, &tmp, h, buf, bufLen, cookie, bufUsed);
}

static int _bDestroy(vfs_driver_t *drv) {
    if (drv) {
        if (drv->ctx)
            WantedFree(drv->ctx);
        WantedFree(drv);
    }
    return 0;
}

vfs_driver_t *PipeDriverCreate(void) {
    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (!drv) return NULL;

    bridge_state_t *s = WantedMalloc(sizeof(*s));
    if (!s) {
        WantedFree(drv);
        return NULL;
    }
    memset(s, 0, sizeof(*s));

    memset(drv, 0, sizeof(*drv));
    drv->bytesId  = 0x65706950; /* 'Pipe' LE */
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx      = (vfs_driver_ctx_t)s;
    drv->Destroy  = _bDestroy;
    drv->Open     = _bOpen;
    drv->Close    = _bClose;
    drv->Read     = _bRead;
    drv->Write    = _bWrite;
    drv->Stat     = _bStat;
    drv->ReadDir  = _bReadDir;
    return drv;
}
