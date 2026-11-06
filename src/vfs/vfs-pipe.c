/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-pipe.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* Named pipe DevFS sub-driver for /dev/pipe/<name>: each pipe is a fixed-size
 * ring in a process-wide store shared by every wapp, which is what makes it an
 * inter-wapp channel. Every access to that store is guarded by store->lock. */

/* Recheck cadence for a blocked call that has no ready event to sleep on. */
#define PIPE_POLL_INTERVAL_NS 1000000ULL /* 1 ms */

typedef struct named_pipe_t {
    char name[PIPE_NAME_MAX + 1];
    uint8_t buf[CONFIG_WANTED_PIPE_BUF_SIZE];
    size_t rpos;
    size_t data_len;
    int writers;
    int readers;
    bool writer_seen; /* a writer has attached at least once → enables EOF */
    /* A console slot's pipe outlives the wapp holding it, so the gap while a
     * writer restarts reads as "nothing yet" rather than end-of-stream. */
    bool persistent;
    /* The last console writer left under its eof option: the stream reads as
     * ended until a writer attaches again. */
    bool end_on_exit;
    /* An eof reader left: writers get -EPIPE until a reader attaches. */
    bool reader_gone;
    bool active;
    /* Ready events of the wapps blocked on this pipe, one entry per wapp. */
    plat_ready_t *waiters[CONFIG_WANTED_MAX_WAPPS];
    size_t waitCnt;
} named_pipe_t;

/* Shared, engine-owned storage. One instance backs every wapp's /dev/pipe. */
struct pipe_store_t {
    named_pipe_t pipes[CONFIG_WANTED_PIPE_MAX_PIPES];
    platform_mutex_t *lock;
};

typedef struct pipe_grant_t {
    char name[PIPE_NAME_MAX + 1];
    bool rd;
    bool wr;
} pipe_grant_t;

/* The names one wapp's driver reaches: launch grants in grants[], the
 * supervisor's runtime clauses in rt[], or every pipe when `all` is set. */
typedef struct pipe_scope_t {
    bool all;
    size_t cnt;
    pipe_grant_t grants[CONFIG_WANTED_PIPE_GRANTS];
    size_t rtCnt;
    pipe_grant_t rt[CONFIG_WANTED_PIPE_RUNTIME_GRANTS];
} pipe_scope_t;

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

pipe_store_t *PipeStoreShared(void) {
    static pipe_store_t *shared = NULL;
    if (!shared)
        shared = PipeStoreNew();
    return shared;
}

/* ── Names and scopes ────────────────────────────────────────────────────────
 */

static bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

int PipeNameCheck(const char *name) {
    if (name == NULL || name[0] == '\0')
        return -EINVAL;
    size_t n = strnlen(name, PIPE_NAME_MAX + 1);
    if (n > PIPE_NAME_MAX)
        return -ENAMETOOLONG;
    for (size_t i = 0; i < n; i++) {
        if (!nameChar(name[i]))
            return -EINVAL;
    }
    return 0;
}

static int scopeAdd(pipe_scope_t *s, const char *name, bool wr) {
    int rc = PipeNameCheck(name);
    if (rc < 0)
        return rc;
    for (size_t i = 0; i < s->cnt; i++) {
        if (strcmp(s->grants[i].name, name) == 0) {
            if (wr)
                s->grants[i].wr = true;
            else
                s->grants[i].rd = true;
            return 0;
        }
    }
    if (s->cnt >= CONFIG_WANTED_PIPE_GRANTS)
        return -ENOSPC;
    pipe_grant_t *g = &s->grants[s->cnt++];
    memcpy(g->name, name, strlen(name) + 1);
    g->rd = !wr;
    g->wr = wr;
    return 0;
}

/* Add the clauses of "read=<name>,write=<name>,..." to `s`. An empty grant adds
 * nothing. On an error `s` is left as it was. */
static int scopeParse(pipe_scope_t *s, const char *grant) {
    char buf[CONFIG_WANTED_MAX_OPTIONS_SIZE];

    if (grant == NULL || grant[0] == '\0')
        return 0;
    size_t len = strnlen(grant, sizeof(buf));
    if (len >= sizeof(buf))
        return -EINVAL;
    memcpy(buf, grant, len + 1);

    pipe_scope_t next = *s;
    for (char *tok = buf; tok != NULL;) {
        char *end = strchr(tok, ',');
        if (end != NULL)
            *end++ = '\0';
        int rc;
        if (strncmp(tok, "read=", 5) == 0)
            rc = scopeAdd(&next, tok + 5, false);
        else if (strncmp(tok, "write=", 6) == 0)
            rc = scopeAdd(&next, tok + 6, true);
        else
            rc = -EINVAL;
        if (rc < 0)
            return rc;
        tok = end;
    }
    *s = next;
    return 0;
}

/* 0 when the scope lets the caller open `name` in the given direction. */
static int scopeCheck(const pipe_scope_t *s, const char *name, bool wr) {
    bool seen = false;

    if (s->all)
        return 0;
    for (size_t i = 0; i < s->cnt; i++) {
        if (strcmp(s->grants[i].name, name) != 0)
            continue;
        if (wr ? s->grants[i].wr : s->grants[i].rd)
            return 0;
        seen = true;
    }
    for (size_t i = 0; i < s->rtCnt; i++) {
        if (strcmp(s->rt[i].name, name) != 0)
            continue;
        if (wr ? s->rt[i].wr : s->rt[i].rd)
            return 0;
        seen = true;
    }
    return seen ? -EACCES : -ENOENT;
}

static bool scopeListed(const pipe_scope_t *s, const char *name) {
    if (s->all)
        return true;
    for (size_t i = 0; i < s->cnt; i++) {
        if (strcmp(s->grants[i].name, name) == 0)
            return true;
    }
    for (size_t i = 0; i < s->rtCnt; i++) {
        if (strcmp(s->rt[i].name, name) == 0)
            return true;
    }
    return false;
}

/* True when a launch grant lets `name` be opened in the direction. */
static bool launchCovers(const pipe_scope_t *s, const char *name, bool wr) {
    for (size_t i = 0; i < s->cnt; i++) {
        if (strcmp(s->grants[i].name, name) == 0)
            return wr ? s->grants[i].wr : s->grants[i].rd;
    }
    return false;
}

static int rtIndex(const pipe_scope_t *s, const char *name, bool wr) {
    for (size_t i = 0; i < s->rtCnt; i++) {
        if (strcmp(s->rt[i].name, name) == 0 &&
            (wr ? s->rt[i].wr : s->rt[i].rd))
            return (int)i;
    }
    return -1;
}

/* Grant or revoke the runtime clauses of "read=<name>,write=<name>,...". The
 * whole list applies or none of it does. */
static int scopeRuntime(pipe_scope_t *s, bool revoke, const char *clauses) {
    char buf[CONFIG_WANTED_MAX_OPTIONS_SIZE];

    if (clauses == NULL || clauses[0] == '\0')
        return -EINVAL;
    size_t len = strnlen(clauses, sizeof(buf));
    if (len >= sizeof(buf))
        return -EINVAL;
    memcpy(buf, clauses, len + 1);

    pipe_scope_t next = *s;
    for (char *tok = buf; tok != NULL;) {
        char *end = strchr(tok, ',');
        bool wr;
        const char *name;
        if (end != NULL)
            *end++ = '\0';
        if (strncmp(tok, "read=", 5) == 0) {
            wr = false;
            name = tok + 5;
        } else if (strncmp(tok, "write=", 6) == 0) {
            wr = true;
            name = tok + 6;
        } else {
            return -EINVAL;
        }
        int rc = PipeNameCheck(name);
        if (rc < 0)
            return rc;
        int at = rtIndex(&next, name, wr);
        if (!revoke) {
            if (next.all || launchCovers(&next, name, wr) || at >= 0) {
                tok = end;
                continue;
            }
            if (next.rtCnt >= CONFIG_WANTED_PIPE_RUNTIME_GRANTS)
                return -ENOSPC;
            pipe_grant_t *g = &next.rt[next.rtCnt++];
            memcpy(g->name, name, strlen(name) + 1);
            g->rd = !wr;
            g->wr = wr;
        } else {
            if (at < 0)
                return launchCovers(&next, name, wr) ? -EPERM : -ENOENT;
            for (size_t i = (size_t)at; i + 1 < next.rtCnt; i++)
                next.rt[i] = next.rt[i + 1];
            next.rtCnt--;
        }
        tok = end;
    }
    *s = next;
    return 0;
}

/* ── Ring helpers (caller holds store->lock) ─────────────────────────────────
 */

static named_pipe_t *findPipe(pipe_store_t *store, const char *name) {
    for (int i = 0; i < CONFIG_WANTED_PIPE_MAX_PIPES; i++) {
        if (store->pipes[i].active &&
            strncmp(store->pipes[i].name, name, PIPE_NAME_MAX + 1) == 0)
            return &store->pipes[i];
    }
    return NULL;
}

static named_pipe_t *allocPipe(pipe_store_t *store, const char *name) {
    for (int i = 0; i < CONFIG_WANTED_PIPE_MAX_PIPES; i++) {
        if (!store->pipes[i].active) {
            named_pipe_t *p = &store->pipes[i];
            memset(p, 0, sizeof(*p));
            strncpy(p->name, name, PIPE_NAME_MAX);
            p->name[PIPE_NAME_MAX] = '\0';
            p->active = true;
            return p;
        }
    }
    return NULL;
}

/* Wake every wapp blocked on `p`. A change that no waiter needs costs the
 * waiter one recheck. */
static void pipeNotify(named_pipe_t *p) {
    for (size_t i = 0; i < p->waitCnt; i++)
        PlatformReadyRaise(p->waiters[i]);
}

static void waitAdd(named_pipe_t *p, plat_ready_t *r) {
    for (size_t i = 0; i < p->waitCnt; i++) {
        if (p->waiters[i] == r)
            return;
    }
    /* A wapp has one ready event and one entry, so the list never fills. */
    if (p->waitCnt < CONFIG_WANTED_MAX_WAPPS)
        p->waiters[p->waitCnt++] = r;
}

static void waitDel(named_pipe_t *p, const plat_ready_t *r) {
    for (size_t i = 0; i < p->waitCnt; i++) {
        if (p->waiters[i] != r)
            continue;
        p->waiters[i] = p->waiters[--p->waitCnt];
        return;
    }
}

/* Sleep until `p` may have changed or the wapp is stopped. The caller added
 * `ready` to the waiters under the lock, so a later change is pending. Without
 * an event the call rechecks every millisecond. */
static int blockOn(pipe_store_t *store, named_pipe_t *p, plat_ready_t *ready,
                   int wakeFd) {
    if (ready == NULL || p == NULL) {
        /* Sleep UNLOCKED so a worker torn down here cannot strand the shared
         * mutex. A signalled stop interrupts the sleep (EINTR); a raised wake
         * descriptor ends the wait at the next check. */
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC,
                                   PIPE_POLL_INTERVAL_NS, 0) == -EINTR)
            return -EINTR;
        return PlatformWakeRaised(wakeFd) ? -EINTR : 0;
    }
    int rc = PlatformReadyWait(ready, -1);
    PlatformMutexLock(store->lock);
    waitDel(p, ready);
    PlatformMutexUnlock(store->lock);
    return rc == -EINTR ? -EINTR : 0;
}

static int ringRead(named_pipe_t *p, void *buf, size_t nbyte) {
    size_t n = nbyte < p->data_len ? nbyte : p->data_len;
    size_t first = CONFIG_WANTED_PIPE_BUF_SIZE - p->rpos;
    if (n <= first) {
        memcpy(buf, p->buf + p->rpos, n);
    } else {
        memcpy(buf, p->buf + p->rpos, first);
        memcpy((uint8_t *)buf + first, p->buf, n - first);
    }
    p->rpos = (p->rpos + n) % CONFIG_WANTED_PIPE_BUF_SIZE;
    p->data_len -= n;
    pipeNotify(p);
    return (int)n;
}

static int ringWrite(named_pipe_t *p, const void *buf, size_t nbyte) {
    size_t space = CONFIG_WANTED_PIPE_BUF_SIZE - p->data_len;
    size_t n = nbyte < space ? nbyte : space;
    if (n == 0)
        return -EAGAIN;

    size_t wpos = (p->rpos + p->data_len) % CONFIG_WANTED_PIPE_BUF_SIZE;
    size_t first = CONFIG_WANTED_PIPE_BUF_SIZE - wpos;
    if (n <= first) {
        memcpy(p->buf + wpos, buf, n);
    } else {
        memcpy(p->buf + wpos, buf, first);
        memcpy(p->buf, (const uint8_t *)buf + first, n - first);
    }
    p->data_len += n;
    pipeNotify(p);
    return (int)n;
}

/* True once every writer has left a pipe that reads end of stream: a plain
 * pipe, or one whose writer ended with eof. */
static bool atEof(const named_pipe_t *p) {
    return p->writer_seen && p->writers <= 0 &&
           (!p->persistent || p->end_on_exit);
}

/* Readiness of one end, mirroring what Read/Write would do. A reader at end of
 * stream is readable and hung up; a writer whose reader left is writable and
 * hung up, so its next write reports the error. */
static int ringPoll(const named_pipe_t *p, bool writer, uint32_t *avail) {
    if (writer) {
        if (p->reader_gone)
            return VFS_POLL_OUT | VFS_POLL_HUP;
        return p->data_len < CONFIG_WANTED_PIPE_BUF_SIZE ? VFS_POLL_OUT : 0;
    }
    *avail = (uint32_t)p->data_len;
    if (p->data_len > 0)
        return VFS_POLL_IN;
    if (atEof(p))
        return VFS_POLL_IN | VFS_POLL_HUP;
    return 0;
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
void *PipeDriver_Open(vfs_ctx_t c, const vfs_driver_t *drv,
                      const pipe_scope_t *scope, const char *suffix,
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

    int rc = PipeNameCheck(suffix);
    if (rc == 0 && (flags & VFS_O_RDWR) == VFS_O_RDWR)
        rc = -EINVAL;
    if (rc == 0 && scope != NULL) {
        PlatformMutexLock(store->lock);
        rc = scopeCheck(scope, suffix, (flags & 3) == VFS_O_WRONLY);
        PlatformMutexUnlock(store->lock);
    }
    if (rc < 0) {
        WantedFree(h);
        if (out_err)
            *out_err = rc;
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
        p->reader_gone = false;
    }
    pipeNotify(p);
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
        pipeNotify(p);
        /* Keep the pipe alive while data is pending so a later reader (possibly
         * in another wapp) can still drain it. */
        if (p->writers <= 0 && p->readers <= 0 && p->data_len == 0)
            p->active = false;
        PlatformMutexUnlock(store->lock);
    }
    WantedFree(h);
    return 0;
}

int PipeDriver_Read(int wakeFd, plat_ready_t *ready, const vfs_driver_t *drv,
                    void *handle, void *buf, size_t nbyte) {
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    named_pipe_t *p = h->pipe;
    bool nonblock = (h->flags & VFS_O_NONBLOCK) != 0;

    for (;;) {
        PlatformMutexLock(store->lock);
        if (p->data_len > 0) {
            int n = ringRead(p, buf, nbyte);
            PlatformMutexUnlock(store->lock);
            return n;
        }
        /* No data buffered. EOF only once a writer has existed, all closed,
         * and the pipe is not a console's; anything else is a would-block. */
        if (atEof(p)) {
            PlatformMutexUnlock(store->lock);
            return 0;
        }
        if (nonblock) {
            PlatformMutexUnlock(store->lock);
            return -EAGAIN;
        }
        if (ready != NULL)
            waitAdd(p, ready);
        PlatformMutexUnlock(store->lock);

        int rc = blockOn(store, p, ready, wakeFd);
        if (rc < 0)
            return rc;
    }
}

int PipeDriver_Write(int wakeFd, plat_ready_t *ready, const vfs_driver_t *drv,
                     void *handle, const void *buf, size_t nbyte) {
    pipe_handle_t *h = handle;
    if (!h || h->is_root || !h->pipe)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    named_pipe_t *p = h->pipe;
    bool nonblock = (h->flags & VFS_O_NONBLOCK) != 0;

    for (;;) {
        PlatformMutexLock(store->lock);
        int n = p->reader_gone ? -EPIPE : ringWrite(p, buf, nbyte);
        if (n != -EAGAIN || nonblock) {
            PlatformMutexUnlock(store->lock);
            return n;
        }
        /* Full ring: a blocking writer waits for the reader to drain. */
        if (ready != NULL)
            waitAdd(p, ready);
        PlatformMutexUnlock(store->lock);

        int rc = blockOn(store, p, ready, wakeFd);
        if (rc < 0)
            return rc;
    }
}

static int pipePoll(vfs_ctx_t c, const vfs_driver_t *drv, void *handle,
                    uint32_t *avail) {
    (void)c;
    const pipe_handle_t *h = handle;
    if (!h)
        return -EBADF;
    if (h->is_root || !h->pipe)
        return VFS_POLL_IN | VFS_POLL_OUT;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    PlatformMutexLock(store->lock);
    int r = ringPoll(h->pipe, (h->flags & 3) == VFS_O_WRONLY, avail);
    PlatformMutexUnlock(store->lock);
    return r;
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

int PipeDriver_ReadDir(vfs_ctx_t c, const vfs_driver_t *drv,
                       const pipe_scope_t *scope, void *handle, void *buf,
                       size_t bufLen, uint64_t *cookie, size_t *bufUsed) {
    (void)c;
    const pipe_handle_t *h = handle;
    if (!h || !h->is_root)
        return -EBADF;

    pipe_store_t *store = (pipe_store_t *)drv->ctx;
    vfs_dir_entry_t entries[CONFIG_WANTED_PIPE_MAX_PIPES];
    size_t count = 0;
    /* Hold the lock across the snapshot AND the serialisation: entries[].name
     * point into store->pipes[i].name, valid only while locked. */
    PlatformMutexLock(store->lock);
    for (int i = 0; i < CONFIG_WANTED_PIPE_MAX_PIPES; i++) {
        if (store->pipes[i].active &&
            (scope == NULL || scopeListed(scope, store->pipes[i].name))) {
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

/* The pipe driver is registered in DevFS under "pipe". DevFS calls the generic
 * vfs_driver_t interface with integer fds, so this wrapper maps an fd to a heap
 * pipe_handle_t through a per-driver table and forwards to the typed calls. */

#define BRIDGE_MAX_HANDLES 16

typedef struct bridge_state_t {
    pipe_store_t *store; /* shared, engine-owned; NOT freed here */
    pipe_scope_t *scope; /* what the driver reaches; never NULL */
    void *handles[BRIDGE_MAX_HANDLES];
    int wakeFd;          /* the wapp's wake descriptor; -1 for none */
    plat_ready_t *ready; /* the wapp's ready event; NULL for none */
} bridge_state_t;

/* The running wapps' pipe drivers by wapp name, for the supervisor's runtime
 * grants. Guarded by the shared store's lock; a driver's scope is guarded by
 * its own store's lock, taken second. */
static struct {
    char wapp[WAPP_MAX_NAME_LEN];
    bridge_state_t *state;
} binds[CONFIG_WANTED_MAX_WAPPS];

static platform_mutex_t *bindsLock(void) {
    pipe_store_t *shared = PipeStoreShared();
    return shared != NULL ? shared->lock : NULL;
}

static void unbindState(const bridge_state_t *s) {
    platform_mutex_t *lock = bindsLock();
    PlatformMutexLock(lock);
    for (size_t i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (binds[i].state == s)
            memset(&binds[i], 0, sizeof(binds[i]));
    }
    PlatformMutexUnlock(lock);
}

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
    void *h = PipeDriver_Open(NULL, &tmp, s->scope, path, flags, &out_err);
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
    return PipeDriver_Read(s->wakeFd, s->ready, &tmp, h, buf, nbyte);
}

static int _bWrite(vfs_driver_ctx_t dctx, int fd, const void *buf,
                   size_t nbyte) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_Write(s->wakeFd, s->ready, &tmp, h, buf, nbyte);
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

static int _bPoll(vfs_driver_ctx_t dctx, int fd, uint32_t *avail) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return pipePoll(NULL, &tmp, h, avail);
}

static int _bSetFlags(vfs_driver_ctx_t dctx, int fd, vfs_oflags_t flags) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    pipe_handle_t *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    h->flags = (h->flags & ~VFS_O_NONBLOCK) | (flags & VFS_O_NONBLOCK);
    return 0;
}

static void _bSetWake(vfs_driver_ctx_t dctx, int fd) {
    ((bridge_state_t *)dctx)->wakeFd = fd;
}

static void _bSetReady(vfs_driver_ctx_t dctx, plat_ready_t *ready) {
    ((bridge_state_t *)dctx)->ready = ready;
}

static void _bWatch(vfs_driver_ctx_t dctx, int fd, plat_ready_t *ready,
                    bool arm) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    pipe_handle_t *h = getHandle(s, fd);

    if (h == NULL || h->pipe == NULL || ready == NULL)
        return;
    PlatformMutexLock(s->store->lock);
    if (arm)
        waitAdd(h->pipe, ready);
    else
        waitDel(h->pipe, ready);
    PlatformMutexUnlock(s->store->lock);
}

static int _bReadDir(vfs_driver_ctx_t dctx, int fd, void *buf, size_t bufLen,
                     uint64_t *cookie, size_t *bufUsed) {
    bridge_state_t *s = (bridge_state_t *)dctx;
    void *h = getHandle(s, fd);
    if (!h)
        return -EBADF;
    vfs_driver_t tmp;
    buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
    return PipeDriver_ReadDir(NULL, &tmp, s->scope, h, buf, bufLen, cookie,
                              bufUsed);
}

static int _bDestroy(vfs_driver_t *drv) {
    if (!drv)
        return 0;
    bridge_state_t *s = (bridge_state_t *)drv->ctx;
    if (s) {
        /* Close any handles this wapp left open before freeing its table. An
         * exiting writer that leaked its `writers` count would keep readers in
         * other wapps from ever reaching EOF. */
        vfs_driver_t tmp;
        buildTmpDriver(&tmp, (vfs_driver_ctx_t)s->store);
        for (int i = 0; i < BRIDGE_MAX_HANDLES; i++) {
            if (s->handles[i]) {
                PipeDriver_Close(NULL, &tmp, s->handles[i]);
                s->handles[i] = NULL;
            }
        }
        unbindState(s);
        WantedFree(s->scope);
        WantedFree(s); /* the store is engine-owned and outlives the driver */
    }
    WantedFree(drv);
    return 0;
}

/* Pipe console driver — backs a console slot with a named pipe in the shared
 * store, so a peer wapp reads the stream live. A full ring short-writes, then
 * blocks to the poll cap: a slow reader throttles the writer. */

typedef struct {
    pipe_store_t *store;
    char name[PIPE_NAME_MAX + 1];
    bool forRead;
    pipe_eof_t eof;
    const int32_t *exitCode; /* the wapp's, read when the console detaches */
    int flags;
    int wakeFd;          /* the wapp's wake descriptor; -1 for none */
    plat_ready_t *ready; /* the wapp's ready event; NULL for none */
} pipe_console_t;

/* Whether the stream ends now that the wapp has: `eof=ok` only on exit 0. */
static bool consoleEnds(const pipe_console_t *c) {
    if (c->eof == PIPE_EOF_ANY)
        return true;
    return c->eof == PIPE_EOF_OK && c->exitCode != NULL && *c->exitCode == 0;
}

static void consoleDetach(pipe_console_t *c) {
    bool ends = consoleEnds(c);
    PlatformMutexLock(c->store->lock);
    named_pipe_t *p = findPipe(c->store, c->name);
    if (p) {
        if (c->forRead) {
            if (p->readers > 0)
                p->readers--;
            if (ends && p->readers <= 0) {
                p->data_len = 0;
                p->rpos = 0;
                p->reader_gone = true;
            }
        } else {
            if (p->writers > 0)
                p->writers--;
            if (ends)
                p->end_on_exit = true;
        }
        pipeNotify(p);
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

    bool nonblock = (c->flags & VFS_O_NONBLOCK) != 0;

    for (;;) {
        PlatformMutexLock(c->store->lock);
        named_pipe_t *p = findPipe(c->store, c->name);
        if (!p)
            p = allocPipe(c->store, c->name);
        int n = -ENOSPC;
        if (p != NULL)
            n = p->reader_gone ? -EPIPE : ringWrite(p, buf, nbyte);
        if (n != -EAGAIN || nonblock) {
            PlatformMutexUnlock(c->store->lock);
            return n;
        }
        /* Full ring: a blocking writer waits for the reader to drain. */
        if (c->ready != NULL)
            waitAdd(p, c->ready);
        PlatformMutexUnlock(c->store->lock);

        int rc = blockOn(c->store, p, c->ready, c->wakeFd);
        if (rc < 0)
            return rc;
    }
}

static int _pcRead(vfs_driver_ctx_t dctx, int fd, void *buf, size_t nbyte) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;
    if (buf == NULL)
        return -EINVAL;
    if (!c->forRead)
        return 0; /* out/err read as EOF */

    bool nonblock = (c->flags & VFS_O_NONBLOCK) != 0;

    for (;;) {
        PlatformMutexLock(c->store->lock);
        named_pipe_t *p = findPipe(c->store, c->name);
        if (p && p->data_len > 0) {
            int n = ringRead(p, buf, nbyte);
            PlatformMutexUnlock(c->store->lock);
            return n;
        }
        if (p && atEof(p)) {
            PlatformMutexUnlock(c->store->lock);
            return 0;
        }
        if (nonblock) {
            PlatformMutexUnlock(c->store->lock);
            return -EAGAIN;
        }
        if (p != NULL && c->ready != NULL)
            waitAdd(p, c->ready);
        PlatformMutexUnlock(c->store->lock);

        int rc = blockOn(c->store, p, c->ready, c->wakeFd);
        if (rc < 0)
            return rc;
    }
}

static void _pcSetWake(vfs_driver_ctx_t dctx, int fd) {
    ((pipe_console_t *)dctx)->wakeFd = fd;
}

static void _pcSetReady(vfs_driver_ctx_t dctx, plat_ready_t *ready) {
    ((pipe_console_t *)dctx)->ready = ready;
}

static void _pcWatch(vfs_driver_ctx_t dctx, int fd, plat_ready_t *ready,
                     bool arm) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;

    if (ready == NULL)
        return;
    PlatformMutexLock(c->store->lock);
    named_pipe_t *p = findPipe(c->store, c->name);
    if (p != NULL) {
        if (arm)
            waitAdd(p, ready);
        else
            waitDel(p, ready);
    }
    PlatformMutexUnlock(c->store->lock);
}

static int _pcPoll(vfs_driver_ctx_t dctx, int fd, uint32_t *avail) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;
    PlatformMutexLock(c->store->lock);
    const named_pipe_t *p = findPipe(c->store, c->name);
    /* A missing pipe is created by the first write, so it reads as writable. */
    int r = c->forRead ? 0 : VFS_POLL_OUT;
    if (p)
        r = ringPoll(p, !c->forRead, avail);
    PlatformMutexUnlock(c->store->lock);
    return r;
}

static int _pcSetFlags(vfs_driver_ctx_t dctx, int fd, vfs_oflags_t flags) {
    (void)fd;
    pipe_console_t *c = (pipe_console_t *)dctx;
    c->flags = (c->flags & ~VFS_O_NONBLOCK) | (flags & VFS_O_NONBLOCK);
    return 0;
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
                                   bool forRead, vfs_oflags_t flags,
                                   pipe_eof_t eof, const int32_t *exitCode) {
    if (!store || PipeNameCheck(name) < 0)
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
    memcpy(c->name, name, strlen(name) + 1);
    c->forRead = forRead;
    c->eof = eof;
    c->exitCode = exitCode;
    c->flags = flags;
    c->wakeFd = -1;

    /* Attach now so the pipe exists and is discoverable before the wapp's first
     * write, and a peer reader sees writer_seen for clean EOF. The attach keeps
     * the pipe in the store for the console's lifetime. */
    PlatformMutexLock(store->lock);
    named_pipe_t *p = findPipe(store, c->name);
    if (!p)
        p = allocPipe(store, c->name);
    if (!p) {
        PlatformMutexUnlock(store->lock);
        WantedFree(c);
        WantedFree(drv);
        return NULL;
    }
    p->persistent = true;
    if (forRead) {
        p->readers++;
        p->reader_gone = false;
    } else {
        p->writers++;
        p->writer_seen = true;
        /* A returning writer resumes the stream, whatever its options. */
        p->end_on_exit = false;
    }
    pipeNotify(p);
    PlatformMutexUnlock(store->lock);

    memset(drv, 0, sizeof(*drv));
    drv->bytesId = 0x736e6f43; /* 'Cons' LE */
    drv->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    drv->ctx = (vfs_driver_ctx_t)c;
    drv->Destroy = _pcDestroy;
    drv->SetWake = _pcSetWake;
    drv->SetReady = _pcSetReady;
    drv->PollWatch = _pcWatch;
    drv->raisesReady = true;
    drv->Close = _pcClose;
    drv->Read = _pcRead;
    drv->Write = _pcWrite;
    drv->Stat = _pcStat;
    drv->Poll = _pcPoll;
    drv->SetFlags = _pcSetFlags;
    return drv;
}

static vfs_driver_t *createDriver(pipe_store_t *store, pipe_scope_t *scope) {
    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (!drv) {
        WantedFree(scope);
        return NULL;
    }

    bridge_state_t *s = WantedMalloc(sizeof(*s));
    if (!s) {
        WantedFree(scope);
        WantedFree(drv);
        return NULL;
    }
    memset(s, 0, sizeof(*s));
    s->store = store;
    s->scope = scope;
    s->wakeFd = -1;

    memset(drv, 0, sizeof(*drv));
    drv->bytesId = 0x65706950; /* 'Pipe' LE */
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx = (vfs_driver_ctx_t)s;
    drv->Destroy = _bDestroy;
    drv->SetWake = _bSetWake;
    drv->SetReady = _bSetReady;
    drv->PollWatch = _bWatch;
    drv->raisesReady = true;
    drv->Open = _bOpen;
    drv->Close = _bClose;
    drv->Read = _bRead;
    drv->Write = _bWrite;
    drv->Stat = _bStat;
    drv->ReadDir = _bReadDir;
    drv->Poll = _bPoll;
    drv->SetFlags = _bSetFlags;
    return drv;
}

vfs_driver_t *PipeDriverCreate(pipe_store_t *store) {
    if (!store)
        return NULL;
    pipe_scope_t *scope = WantedMalloc(sizeof(*scope));
    if (!scope)
        return NULL;
    memset(scope, 0, sizeof(*scope));
    scope->all = true;
    return createDriver(store, scope);
}

vfs_driver_t *PipeDriverCreateScoped(pipe_store_t *store, const char *grant) {
    if (!store)
        return NULL;
    pipe_scope_t *scope = WantedMalloc(sizeof(*scope));
    if (!scope)
        return NULL;
    memset(scope, 0, sizeof(*scope));
    if (scopeParse(scope, grant) < 0) {
        WantedFree(scope);
        return NULL;
    }
    return createDriver(store, scope);
}

int PipeDriverGrant(vfs_driver_t *drv, const char *grant) {
    if (drv == NULL || drv->Open != _bOpen)
        return -EINVAL;
    bridge_state_t *s = (bridge_state_t *)drv->ctx;
    if (s->scope == NULL || s->scope->all)
        return -EINVAL;
    return scopeParse(s->scope, grant);
}

int PipeDriverBind(vfs_driver_t *drv, const char *wapp) {
    if (drv == NULL || drv->Open != _bOpen || wapp == NULL || wapp[0] == '\0' ||
        strnlen(wapp, WAPP_MAX_NAME_LEN) >= WAPP_MAX_NAME_LEN)
        return -EINVAL;
    bridge_state_t *s = (bridge_state_t *)drv->ctx;
    platform_mutex_t *lock = bindsLock();
    int rc = -ENOSPC;

    PlatformMutexLock(lock);
    for (size_t i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (binds[i].state != NULL && strcmp(binds[i].wapp, wapp) == 0) {
            memset(&binds[i], 0, sizeof(binds[i]));
            break;
        }
    }
    for (size_t i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (binds[i].state == NULL) {
            memcpy(binds[i].wapp, wapp, strlen(wapp) + 1);
            binds[i].state = s;
            rc = 0;
            break;
        }
    }
    PlatformMutexUnlock(lock);
    return rc;
}

static bridge_state_t *boundState(const char *wapp) {
    for (size_t i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (binds[i].state != NULL && strcmp(binds[i].wapp, wapp) == 0)
            return binds[i].state;
    }
    return NULL;
}

/* Lock a bound driver's scope, under the binds lock the caller holds. */
static void scopeLock(const bridge_state_t *s, bool lock) {
    if (s->store == PipeStoreShared())
        return;
    if (lock)
        PlatformMutexLock(s->store->lock);
    else
        PlatformMutexUnlock(s->store->lock);
}

int PipeRuntimeGrant(const char *wapp, bool revoke, const char *clauses) {
    if (wapp == NULL)
        return -EINVAL;
    platform_mutex_t *lock = bindsLock();
    PlatformMutexLock(lock);
    bridge_state_t *s = boundState(wapp);
    int rc = -ENOENT;
    if (s != NULL) {
        scopeLock(s, true);
        rc = scopeRuntime(s->scope, revoke, clauses);
        scopeLock(s, false);
    }
    PlatformMutexUnlock(lock);
    return rc;
}

static int runtimeList(const pipe_scope_t *scope, char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (size_t i = 0; i < scope->rtCnt; i++) {
        const pipe_grant_t *g = &scope->rt[i];
        int w = snprintf(buf + used, cap - used, "%s=%s\n",
                         g->wr ? "write" : "read", g->name);
        if (w < 0 || (size_t)w >= cap - used)
            return -EMSGSIZE;
        used += (size_t)w;
    }
    return (int)used;
}

int PipeRuntimeList(const char *wapp, char *buf, size_t cap) {
    if (wapp == NULL || buf == NULL || cap == 0)
        return -EINVAL;
    platform_mutex_t *lock = bindsLock();
    PlatformMutexLock(lock);
    const bridge_state_t *s = boundState(wapp);
    int rc = -ENOENT;
    if (s != NULL) {
        scopeLock(s, true);
        rc = runtimeList(s->scope, buf, cap);
        scopeLock(s, false);
    }
    PlatformMutexUnlock(lock);
    return rc;
}

/* `pipe` is listed and reserved in the driver table, but the launch installs
 * it only from drivers[] and console slots, so any other placement fails. */
vfs_driver_t *VfsPipeInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;
    return NULL;
}

int PipeConsoleOptions(const char *options, char *name, size_t cap,
                       pipe_eof_t *eof) {
    *eof = PIPE_EOF_NONE;
    name[0] = '\0';
    if (options == NULL)
        return 0;

    const char *p = options;
    while (*p != '\0') {
        size_t n = strcspn(p, ",");
        if (n == 3 && strncmp(p, "eof", 3) == 0) {
            *eof = PIPE_EOF_ANY;
        } else if (n == 6 && strncmp(p, "eof=ok", 6) == 0) {
            *eof = PIPE_EOF_OK;
        } else if (n >= 5 && strncmp(p, "name=", 5) == 0) {
            if (n - 5 >= cap)
                return -ENAMETOOLONG;
            memcpy(name, p + 5, n - 5);
            name[n - 5] = '\0';
            int rc = PipeNameCheck(name);
            if (rc < 0)
                return rc;
        } else {
            return -EINVAL;
        }
        p += n;
        if (*p == ',')
            p++;
    }
    return 0;
}
