/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-pipe.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Named pipe DevFS sub-driver for /dev/pipe/<name>.
 *
 * Each named pipe is a fixed-size ring buffer. Write creates the pipe if
 * absent; read creates it too (idempotent). A pipe is destroyed when the last
 * reader and last writer close and no data remains buffered. The driver itself
 * is a directory driver registered under the name "pipe" in DevFS; DevFS
 * prefix-matches "/dev/pipe/t" → sub_path "t" and calls PipeDriver_Open("t").
 *
 * The ring buffers live in a process-wide pipe_store_t shared by every wapp's
 * driver instance, so a pipe opened by one wapp is visible to another — this is
 * the inter-wapp IPC channel. Every access to that shared storage is bracketed
 * by store->lock. The per-wapp handle table (in bridge_state_t) is touched only
 * by its own wapp's thread and needs no lock. */

#define PIPE_BUF_SIZE 4096
#define PIPE_MAX_PIPES 8

/* Blocking-read poll cadence. A read with no data sleeps unlocked between
 * rechecks (cancellation-safe, unlike a cond_wait that could be cancelled while
 * holding the lock). The cap bounds the wait so a never-arriving peer becomes a
 * test failure (-EAGAIN) rather than a process hang. */
#define PIPE_POLL_INTERVAL_NS 1000000ULL /* 1 ms */
#define PIPE_POLL_MAX_ITERS 5000         /* ~5 s safety cap */

typedef struct named_pipe_t {
    char name[MAX_ENTRY_NAME_LEN];
    uint8_t buf[PIPE_BUF_SIZE];
    size_t rpos;
    size_t data_len;
    int writers;
    int readers;
    bool writer_seen; /* a writer has attached at least once → enables EOF */
    bool active;
} named_pipe_t;

/* Shared, engine-owned storage. One instance backs every wapp's /dev/pipe. */
struct pipe_store_t {
    named_pipe_t pipes[PIPE_MAX_PIPES];
    platform_mutex_t *lock;
};

typedef struct pipe_handle_t {
    named_pipe_t *pipe; /* NULL for root directory handle */
    int flags;
    bool is_root;
} pipe_handle_t;

/* ── Shared store lifecycle ──────────────────────────────────────────────────
 */

pipe_store_t *PipeStoreNew(void) {
    pipe_store_t *store = WantedMalloc(sizeof(*store));
    if (!store)
        return NULL;
    memset(store, 0, sizeof(*store));
    store->lock = PlatformMutexNew(); /* NULL-tolerant downstream */
    return store;
}

void PipeStoreFree(pipe_store_t *store) {
    if (!store)
        return;
    PlatformMutexFree(store->lock);
    WantedFree(store);
}

/* ── Ring helpers (caller holds store->lock) ─────────────────────────────────
 */

static named_pipe_t *findPipe(pipe_store_t *store, const char *name) {
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (store->pipes[i].active &&
            strncmp(store->pipes[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return &store->pipes[i];
    }
    return NULL;
}

static named_pipe_t *allocPipe(pipe_store_t *store, const char *name) {
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (!store->pipes[i].active) {
            named_pipe_t *p = &store->pipes[i];
            memset(p, 0, sizeof(*p));
            strncpy(p->name, name, MAX_ENTRY_NAME_LEN - 1);
            p->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
            p->active = true;
            return p;
        }
    }
    return NULL;
}

static int ringRead(named_pipe_t *p, void *buf, size_t nbyte) {
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

static int ringWrite(named_pipe_t *p, const void *buf, size_t nbyte) {
    size_t space = PIPE_BUF_SIZE - p->data_len;
    size_t n = nbyte < space ? nbyte : space;
    if (n == 0)
        return -EAGAIN;

    size_t wpos = (p->rpos + p->data_len) % PIPE_BUF_SIZE;
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

/* Template used by bridge helpers to forward calls into the typed API. */
static void buildTmpDriver(vfs_driver_t *drv, vfs_driver_ctx_t ctx) {
    memset(drv, 0, sizeof(*drv));
    drv->bytesId = 0x65706950;
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx = ctx;
}

/* ── Public DevFS-sub-driver interface ───────────────────────────────────────
 */

/* Called by DevFS with the suffix AFTER the "pipe" prefix, e.g. "t" or "/".
 * drv->ctx is the shared pipe_store_t. */
void *PipeDriver_Open(vfs_ctx_t c, const vfs_driver_t *drv, const char *suffix,
                      vfs_oflags_t flags, int *out_err) {
    (void)c;
    pipe_store_t *store = (pipe_store_t *)drv->ctx;

    pipe_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        if (out_err)
            *out_err = -ENOMEM;
        return NULL;
    }

    /* Empty suffix or "/" → root directory open (ls /dev/pipe). */
    if (!suffix || *suffix == '\0' || strcmp(suffix, "/") == 0) {
        h->pipe = NULL;
        h->flags = flags;
        h->is_root = true;
        if (out_err)
            *out_err = 0;
        return h;
    }

    if ((flags & VFS_O_RDWR) == VFS_O_RDWR) {
        WantedFree(h);
        if (out_err)
            *out_err = -EINVAL;
        return NULL;
    }

    PlatformMutexLock(store->lock);
    named_pipe_t *p = findPipe(store, suffix);
    if (!p)
        p = allocPipe(store, suffix);
    if (!p) {
        PlatformMutexUnlock(store->lock);
        WantedFree(h);
        if (out_err)
            *out_err = -ENOSPC;
        return NULL;
    }

    if ((flags & 3) == VFS_O_WRONLY) {
        p->writers++;
        /* As soon as any writer attaches, a later reader's EOF becomes
         * meaningful: writer_seen distinguishes "all writers closed" (EOF)
         * from "no writer has ever attached" (keep blocking). */
        p->writer_seen = true;
    } else {
        p->readers++;
    }
    PlatformMutexUnlock(store->lock);

    h->pipe = p;
    h->flags = flags;
    h->is_root = false;
    if (out_err)
        *out_err = 0;
    return h;
}

int PipeDriver_Close(vfs_ctx_t c, const vfs_driver_t *drv, void *handle) {
    (void)c;
    pipe_handle_t *h = handle;
    if (!h)
        return -EBADF;

    if (!h->is_root && h->pipe) {
        pipe_store_t *store = (pipe_store_t *)drv->ctx;
        named_pipe_t *p = h->pipe;
        PlatformMutexLock(store->lock);
        if ((h->flags & 3) == VFS_O_WRONLY)
            p->writers--;
        else
            p->readers--;
        /* Keep the pipe alive while data is pending so a later reader (possibly
         * in another wapp) can still drain it. */
        if (p->writers <= 0 && p->readers <= 0 && p->data_len == 0)
            p->active = false;
        PlatformMutexUnlock(store->lock);
    }
    WantedFree(h);
    return 0;
}

int PipeDriver_Read(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                    void *buf, size_t nbyte) {
    (void)c;
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    named_pipe_t *p = h->pipe;
    bool nonblock = (h->flags & VFS_O_NONBLOCK) != 0;

    for (size_t iter = 0;; iter++) {
        PlatformMutexLock(store->lock);
        if (p->data_len > 0) {
            int n = ringRead(p, buf, nbyte);
            PlatformMutexUnlock(store->lock);
            return n;
        }
        /* No data buffered. EOF only once a writer has existed and all closed;
         * otherwise this is a would-block (a writer is attached, or none has
         * ever attached and we wait for the first). */
        if (p->writer_seen && p->writers <= 0) {
            PlatformMutexUnlock(store->lock);
            return 0;
        }
        PlatformMutexUnlock(store->lock);

        if (nonblock || iter >= PIPE_POLL_MAX_ITERS)
            return -EAGAIN;
        /* Sleep UNLOCKED: async pthread_cancel on a worker thread must never
         * strand the shared mutex. A signalled stop interrupts the sleep
         * (EINTR); return it so the read unwinds to the interpreter and the
         * terminate flag is honoured, instead of polling out the safety cap. */
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC,
                                   PIPE_POLL_INTERVAL_NS, 0) == -EINTR)
            return -EINTR;
    }
}

int PipeDriver_Write(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                     const void *buf, size_t nbyte) {
    (void)c;
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    named_pipe_t *p = h->pipe;
    PlatformMutexLock(store->lock);
    int n = ringWrite(p, buf, nbyte);
    PlatformMutexUnlock(store->lock);
    return n;
}

int PipeDriver_Stat(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                    vfs_stat_t *stat) {
    (void)c;
    (void)drv;
    pipe_handle_t *h = handle;
    if (!h)
        return -EBADF;
    memset(stat, 0, sizeof(*stat));
    stat->filetype =
        h->is_root ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

int PipeDriver_ReadDir(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                       void *buf, size_t bufLen, uint64_t *cookie,
                       size_t *bufUsed) {
    (void)c;
    const pipe_handle_t *h = handle;
    if (!h || !h->is_root)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    vfs_dir_entry_t entries[PIPE_MAX_PIPES];
    size_t count = 0;
    /* Hold the lock across the snapshot AND the serialisation: entries[].name
     * point into store->pipes[i].name, valid only while locked. */
    PlatformMutexLock(store->lock);
    for (int i = 0; i < PIPE_MAX_PIPES; i++) {
        if (store->pipes[i].active) {
            entries[count].name = store->pipes[i].name;
            entries[count].type = VFS_FILETYPE_CHARACTER_DEVICE;
            count++;
        }
    }
    int r = VfsFlatDirReadDir(entries, count, buf, bufLen, cookie, bufUsed);
    PlatformMutexUnlock(store->lock);
    return r;
}

/* ── DevFS integration bridge ────────────────────────────────────────────────
 */

/* The pipe driver is registered in DevFS under the name "pipe". DevFS calls the
 * generic vfs_driver_t interface with integer fds, so this thin wrapper maps an
 * fd to a heap pipe_handle_t through a per-driver handle table and forwards to
 * the typed functions above. The shared pipe_store_t is passed through a
 * temporary vfs_driver_t whose ctx is the store. */

#define BRIDGE_MAX_HANDLES 16

typedef struct bridge_state_t {
    pipe_store_t *store; /* shared, engine-owned; NOT freed here */
    void *handles[BRIDGE_MAX_HANDLES];
} bridge_state_t;

static int allocHandle(bridge_state_t *s, void *h) {
    for (int i = 0; i < BRIDGE_MAX_HANDLES; i++) {
        if (!s->handles[i]) {
            s->handles[i] = h;
            return i;
        }
    }
    return -EMFILE;
}

static void freeHandle(bridge_state_t *s, int fd) {
    if (fd >= 0 && fd < BRIDGE_MAX_HANDLES)
        s->handles[fd] = NULL;
}

static void *getHandle(bridge_state_t *s, int fd) {
    if (fd < 0 || fd >= BRIDGE_MAX_HANDLES)
        return NULL;
    return s->handles[fd];
}

static int _bOpen(vfs_driver_ctx_t dctx, const char *path, vfs_oflags_t flags) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    int out_err = 0;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    void *h = PipeDriver_Open(NULL, &tmp, path, flags, &out_err);
    if (!h)
        return out_err;
    int fd = allocHandle(s, h);
    if (fd < 0) {
        PipeDriver_Close(NULL, &tmp, h);
        return fd;
    }
    return fd;
}

static int _bClose(vfs_driver_ctx_t dctx, int fd) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    int r = PipeDriver_Close(NULL, &tmp, h);
    freeHandle(s, fd);
    return r;
}

static int _bRead(vfs_driver_ctx_t dctx, int fd, void *buf, size_t nbyte) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_Read(NULL, &tmp, h, buf, nbyte);
}

static int _bWrite(vfs_driver_ctx_t dctx, int fd, const void *buf,
                   size_t nbyte) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_Write(NULL, &tmp, h, buf, nbyte);
}

static int _bStat(vfs_driver_ctx_t dctx, int fd, vfs_stat_t *stat) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_Stat(NULL, &tmp, h, stat);
}

static int _bReadDir(vfs_driver_ctx_t dctx, int fd, void *buf, size_t bufLen,
                     uint64_t *cookie, size_t *bufUsed) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_ReadDir(NULL, &tmp, h, buf, bufLen, cookie, bufUsed);
}

static int _bDestroy(vfs_driver_t *drv) {
    if (!drv)
        return 0;
    bridge_state_t *s = (bridge_state_t *)drv->ctx;
    if (s) {
        /* Close any handles this wapp left open before freeing its table.
         * Sharing the store makes this mandatory: an exiting writer that leaks
         * its `writers` count would keep readers in other wapps from ever
         * reaching EOF. PipeDriver_Close takes store->lock per handle. */
        vfs_driver_t tmp;
        buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
        for (int i = 0; i < BRIDGE_MAX_HANDLES; i++) {
            if (s->handles[i]) {
                PipeDriver_Close(NULL, &tmp, s->handles[i]);
                s->handles[i] = NULL;
            }
        }
        WantedFree(s); /* the store is engine-owned and outlives the driver */
    }
    WantedFree(drv);
    return 0;
}

/* ── Pipe console driver ─────────────────────────────────────────────────────
 *
 * Backs a wapp's console slot (in/out/err) with a named pipe in the shared
 * store, so a peer wapp can read the stream live at /dev/pipe/<name>. Bound to
 * one pipe at creation (no Open — VfsRegister installs it directly as the stream
 * fd). out/err are lossy writers: when the ring is full they drop the oldest
 * bytes and report the whole write consumed, so a console with no reader can
 * never wedge the wapp. in is a reader, mirroring PipeDriver_Read. */

typedef struct {
    pipe_store_t *store;
    char name[MAX_ENTRY_NAME_LEN];
    bool forRead;
    int flags;
} pipe_console_t;

/* Caller holds store->lock. Drop the oldest buffered bytes as needed so the
 * most recent `nbyte` (capped to the ring) always fit; never blocks. */
static int ringWriteLossy(named_pipe_t *p, const void *buf, size_t nbyte) {
    const uint8_t *src = (const uint8_t *)buf;
    size_t keep = nbyte;
    if (keep > PIPE_BUF_SIZE) {
        src += keep - PIPE_BUF_SIZE;
        keep = PIPE_BUF_SIZE;
    }
    size_t space = PIPE_BUF_SIZE - p->data_len;
    if (keep > space) {
        size_t drop = keep - space;
        p->rpos = (p->rpos + drop) % PIPE_BUF_SIZE;
        p->data_len -= drop;
    }
    (void)ringWrite(p, src, keep);
    return (int)nbyte; /* lossy: always report the whole write consumed */
}

static void consoleDetach(pipe_console_t *c) {
    PlatformMutexLock(c->store->lock);
    named_pipe_t *p = findPipe(c->store, c->name);
    if (p) {
        if (c->forRead) {
            if (p->readers > 0)
                p->readers--;
        } else if (p->writers > 0) {
            p->writers--;
        }
        if (p->writers <= 0 && p->readers <= 0 && p->data_len == 0)
            p->active = false;
    }
    PlatformMutexUnlock(c->store->lock);
}

static int _pcClose(vfs_driver_ctx_t dctx, int fd) {
    (void)dctx;
    (void)fd;
    return 0; /* detach is done once in Destroy (a console has a single fd) */
}

static int _pcWrite(vfs_driver_ctx_t dctx, int fd, const void *buf,
                    size_t nbyte) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;
    if (buf == NULL)
        return -EINVAL;
    if (c->forRead)
        return -EBADF; /* the `in` console is read-only */

    PlatformMutexLock(c->store->lock);
    named_pipe_t *p = findPipe(c->store, c->name);
    if (!p)
        p = allocPipe(c->store, c->name);
    int n = p ? ringWriteLossy(p, buf, nbyte) : -ENOSPC;
    PlatformMutexUnlock(c->store->lock);
    return n;
}

static int _pcRead(vfs_driver_ctx_t dctx, int fd, void *buf, size_t nbyte) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;
    if (buf == NULL)
        return -EINVAL;
    if (!c->forRead)
        return 0; /* out/err read as EOF */

    bool nonblock = (c->flags & VFS_O_NONBLOCK) != 0;
    for (size_t iter = 0;; iter++) {
        PlatformMutexLock(c->store->lock);
        named_pipe_t *p = findPipe(c->store, c->name);
        if (p && p->data_len > 0) {
            int n = ringRead(p, buf, nbyte);
            PlatformMutexUnlock(c->store->lock);
            return n;
        }
        bool eof = p && p->writer_seen && p->writers <= 0;
        PlatformMutexUnlock(c->store->lock);
        if (eof)
            return 0;
        if (nonblock || iter >= PIPE_POLL_MAX_ITERS)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, PIPE_POLL_INTERVAL_NS,
                                   0) == -EINTR)
            return -EINTR;
    }
}

static int _pcStat(vfs_driver_ctx_t dctx, int fd, vfs_stat_t *stat) {
    (void)dctx;
    (void)fd;
    if (stat == NULL)
        return -EINVAL;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _pcDestroy(vfs_driver_t *drv) {
    if (drv) {
        pipe_console_t *c = (pipe_console_t *)drv->ctx;
        if (c) {
            consoleDetach(c);
            WantedFree(c);
        }
        WantedFree(drv);
    }
    return 0;
}

vfs_driver_t *VfsPipeConsoleCreate(pipe_store_t *store, const char *name,
                                   bool forRead, vfs_oflags_t flags) {
    if (!store || !name || name[0] == '\0')
        return NULL;

    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (!drv)
        return NULL;
    pipe_console_t *c = WantedMalloc(sizeof(*c));
    if (!c) {
        WantedFree(drv);
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->store = store;
    strncpy(c->name, name, MAX_ENTRY_NAME_LEN - 1);
    c->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
    c->forRead = forRead;
    c->flags = flags;

    /* Attach now so the pipe exists and is discoverable before the wapp's first
     * write, and a peer reader sees writer_seen for clean EOF. */
    PlatformMutexLock(store->lock);
    named_pipe_t *p = findPipe(store, c->name);
    if (!p)
        p = allocPipe(store, c->name);
    if (p) {
        if (forRead) {
            p->readers++;
        } else {
            p->writers++;
            p->writer_seen = true;
        }
    }
    PlatformMutexUnlock(store->lock);

    memset(drv, 0, sizeof(*drv));
    drv->bytesId = 0x736e6f43; /* 'Cons' LE */
    drv->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    drv->ctx = (vfs_driver_ctx_t)c;
    drv->Destroy = _pcDestroy;
    drv->Close = _pcClose;
    drv->Read = _pcRead;
    drv->Write = _pcWrite;
    drv->Stat = _pcStat;
    return drv;
}

vfs_driver_t *PipeDriverCreate(pipe_store_t *store) {
    if (!store)
        return NULL;

    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (!drv)
        return NULL;

    bridge_state_t *s = WantedMalloc(sizeof(*s));
    if (!s) {
        WantedFree(drv);
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->store = store;

    memset(drv, 0, sizeof(*drv));
    drv->bytesId = 0x65706950; /* 'Pipe' LE */
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx = (vfs_driver_ctx_t)s;
    drv->Destroy = _bDestroy;
    drv->Open = _bOpen;
    drv->Close = _bClose;
    drv->Read = _bRead;
    drv->Write = _bWrite;
    drv->Stat = _bStat;
    drv->ReadDir = _bReadDir;
    return drv;
}
