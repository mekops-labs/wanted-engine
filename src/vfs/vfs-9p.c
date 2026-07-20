/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#ifdef WANTED_9P_UNIX_TRANSPORT
#include <sys/un.h>
#endif

#include <vfs-drivers.h>
#include <vfs.h>

#include <debug_trace.h>
#include <wanted_malloc.h>

#include <c9.h>

static const char id[] = {'S', 't', 'y', 'x'};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);
static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path);

/*
 * Negotiated 9P transport message size (the Tversion msize). Bounds the read
 * and write buffers; the size[4] length prefix is counted within it.
 */
#define MSIZE 8192u

/*
 * Bytes of an Rread message body that precede the payload: type[1] + tag[2] +
 * count[4]. The read data begins at this offset within rBuf.
 */
#define RREAD_HDR_LEN 7u

/* Connection state, held as a bitmask in C9aux.flags. */
enum conn_state {
    CONN_ERROR = 0x1,        /* last response was an Rerror */
    CONN_ATTACHED = 0x2,     /* session established (Rattach received) */
    CONN_DISCONNECTED = 0x4, /* transport torn down */
};

/* Per-mount open-file table: each 9P mount reserves this many fd slots. */
#define MAX_OPENED_FILES 10

/*
 * Mount address grammar: "<scheme>://<rest>". The inet schemes take
 * "<host>:<port>"; the local scheme takes a filesystem socket path.
 */
#define SCHEME_SEP "://"
#define SCHEME_TCP "tcp"
#define SCHEME_UDP "udp"
#define SCHEME_UNIX "unix"

typedef struct C9aux {
    int f;
    int flags;
    uint8_t rBuf[MSIZE];
    size_t rCnt;
    uint8_t wBuf[MSIZE];
    size_t wOff;
    /* Copied out of the response callback's frame: c9proc hands the parsed
     * C9stat by pointer into its own stack, which is gone by the time _Stat
     * reads it. The string fields still point into rBuf and stay valid until
     * the next round trip; only the scalars are read here. */
    C9stat lastStat;
    uint8_t haveStat;
    C9qid lastQid;
    C9ctx c;
    C9tag tag;
} C9aux;

typedef struct file_t {
    uint8_t opened;
    size_t currOff;
    vfs_filetype_t type;
} file_t;

struct vfs_driver_ctx_t {
    C9aux aux;
    file_t fildes[MAX_OPENED_FILES];
    char *conf;
};

static int findFirstClosedFd(vfs_driver_ctx_t d) {
    if (!d)
        return -EINVAL;

    for (int i = 0; i < MAX_OPENED_FILES; i++) {
        if (!d->fildes[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}

/*
 * Split a '/'-separated path into its components for c9walk. The 9P protocol
 * caps a single walk at C9maxpathel (16) elements, so out must hold that many
 * plus the trailing NULL terminator c9walk requires. buf must be a writable
 * copy of the path (strtok mutates it). Returns the component count, or
 * -ENAMETOOLONG when the path has more elements than one walk can carry.
 */
static int splitPath(char *buf, const char *out[C9maxpathel + 1]) {
    int n = 0;
    for (char *tok = strtok(buf, "/"); tok != NULL; tok = strtok(NULL, "/")) {
        if (n >= C9maxpathel)
            return -ENAMETOOLONG;
        out[n++] = tok;
    }
    out[n] = NULL;
    return n;
}

static vfs_filetype_t convert9pFiletype(C9qt t) {
    if (t & C9qtdir) {
        return VFS_FILETYPE_DIRECTORY;
    }
    if (t & C9qtfile) {
        return VFS_FILETYPE_CHARACTER_DEVICE;
    }
    return VFS_FILETYPE_REGULAR_FILE;
}

static int wrsend(C9aux *a) {
    uint32_t n;
    int w;

    for (n = 0; n < a->wOff; n += w) {
        if ((w = write(a->f, a->wBuf + n, a->wOff - n)) <= 0) {
            if (errno == EINTR)
                continue;
            if (errno != EPIPE) { /* remote end closed */
                perror("write");
                a->flags &= ~CONN_ATTACHED;
                close(a->f);
            }
            return -1;
        }
    }
    a->wOff = 0;

    return 0;
}

/* Signature fixed by the C9ctx callback table. */
/* cppcheck-suppress constParameterCallback */
static uint8_t *ctxbegin(C9ctx *ctx, uint32_t size) {
    uint8_t *b;
    C9aux *a;

    a = ctx->aux;
    if (a->wOff + size > sizeof(a->wBuf)) {
        if (wrsend(a) != 0 || a->wOff + size > sizeof(a->wBuf))
            return NULL;
    }
    b = a->wBuf + a->wOff;
    a->wOff += size;

    return b;
}

static int ctxend(C9ctx *ctx) {
    (void)ctx;
    return 0;
}

/* Signature fixed by the C9ctx callback table. */
/* cppcheck-suppress constParameterCallback */
static uint8_t *ctxread(C9ctx *ctx, uint32_t size, int *err) {
    uint32_t n;
    int r;
    C9aux *a;

    a = ctx->aux;
    *err = 0;
    for (n = 0; n < size; n += r) {
        if ((r = read(a->f, a->rBuf + n, size - n)) <= 0) {
            if (errno == EINTR)
                continue;
            if (errno != EPIPE) { /* remote end closed */
                perror("read");
                a->flags &= ~CONN_ATTACHED;
                close(a->f);
            }
            return NULL;
        }
    }

    return a->rBuf;
}

__attribute__((format(printf, 1, 2))) static void ctxerror(const char *fmt,
                                                           ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* Connect to "<host>:<port>" with the socket type/protocol the scheme picked.
 */
static int dialInet(const char *hostPort, int socktype, int protocol) {
    struct addrinfo *r, *a,
        hint = {.ai_flags = AI_ADDRCONFIG, .ai_family = AF_UNSPEC, 0};
    char host[64];
    const char *colon;
    size_t hostLen;
    int f;

    hint.ai_socktype = socktype;
    hint.ai_protocol = protocol;

    if ((colon = strrchr(hostPort, ':')) == NULL || colon == hostPort) {
        DEBUG_TRACE("invalid 9p address (host/port): %s", hostPort);
        return -1;
    }
    hostLen = (size_t)(colon - hostPort);
    if (hostLen >= sizeof(host)) {
        DEBUG_TRACE("host name too large: %s", hostPort);
        return -1;
    }
    memcpy(host, hostPort, hostLen);
    host[hostLen] = '\0';

    if (getaddrinfo(host, colon + 1, &hint, &r) != 0) {
        return -1;
    }
    f = -1;
    for (a = r; a != NULL; a = a->ai_next) {
        if ((f = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) < 0)
            continue;
        if (connect(f, a->ai_addr, a->ai_addrlen) == 0)
            break;
        close(f);
        f = -1;
    }
    freeaddrinfo(r);

    return f;
}

/*
 * Connect to a driver server listening on a filesystem socket, so an on-box
 * server needs no loopback port. Reachability is then the socket's own
 * filesystem permissions. Compiled in only where the platform offers local
 * sockets — the lwIP-based targets do not.
 */
static int dialLocal(const char *path) {
#ifdef WANTED_9P_UNIX_TRANSPORT
    struct sockaddr_un addr;
    size_t pathLen = strlen(path);
    int f;

    if (pathLen == 0 || pathLen >= sizeof(addr.sun_path)) {
        DEBUG_TRACE("invalid 9p address (socket path): %s", path);
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, pathLen + 1);

    if ((f = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return -1;
    if (connect(f, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(f);
        return -1;
    }

    return f;
#else
    DEBUG_TRACE("9p local transport unavailable on this target: %s", path);
    (void)path;
    return -1;
#endif
}

static int dial(const char *s) {
    const char *sep, *rest;
    size_t schemeLen;

    if ((sep = strstr(s, SCHEME_SEP)) == NULL) {
        DEBUG_TRACE("invalid 9p address (no scheme): %s", s);
        return -1;
    }
    schemeLen = (size_t)(sep - s);
    rest = sep + strlen(SCHEME_SEP);

    if (schemeLen == strlen(SCHEME_TCP) &&
        strncmp(s, SCHEME_TCP, schemeLen) == 0)
        return dialInet(rest, SOCK_STREAM, IPPROTO_TCP);
    if (schemeLen == strlen(SCHEME_UDP) &&
        strncmp(s, SCHEME_UDP, schemeLen) == 0)
        return dialInet(rest, SOCK_DGRAM, IPPROTO_UDP);
    if (schemeLen == strlen(SCHEME_UNIX) &&
        strncmp(s, SCHEME_UNIX, schemeLen) == 0)
        return dialLocal(rest);

    DEBUG_TRACE("invalid 9p address (scheme): %s", s);
    return -1;
}

static void ctxprocR(C9ctx *ctx, C9r *r) {
    C9aux *a;
    C9tag tag;

    a = ctx->aux;
    switch (r->type) {
    case Rversion:
        DEBUG_TRACE("Rversion");
        c9attach(ctx, &tag, 0, C9nofid, "none", NULL);
        a->tag = tag;
        break;

    case Rattach:
        DEBUG_TRACE("Rattach");
        a->flags = CONN_ATTACHED;
        // path[0] = channel;
        // path[1] = NULL;
        // c9walk(ctx, &tag, Rootfid, Chatfid, path);
        // needopen = 1;
        break;

    case Rwalk:
        DEBUG_TRACE("Rwalk");
        // needopen = needopen && c9open(ctx, &tag, Chatfid, C9rdwr);
        break;

    case Rread:
        DEBUG_TRACE("Rread: %lu", r->read.size);
        a->rCnt = r->read.size;
        break;
    case Rstat:
        DEBUG_TRACE("Rstat");
        a->lastStat = r->stat;
        a->haveStat = 1;
        break;
    case Ropen:
        DEBUG_TRACE("Ropen");
        a->lastQid = r->qid[0];
        // if ((a->flags & Joined) == 0 && printjoin) {
        // 	c9write(ctx, &tag, Chatfid, 0, buf, snprintf(buf, sizeof(buf),
        // "JOIN %s to chat\n", nick)); 	a->flags |= Joined;
        // }
        // c9read(ctx, &tag, Chatfid, chatoff, chatoff < skipuntil ?
        // skipuntil-chatoff : Msize);
        break;

    case Rclunk:
        DEBUG_TRACE("Rclunk");
        break;

    case Rerror:
        DEBUG_TRACE("Rerror: %s", r->error);
        a->flags = CONN_ERROR;
        break;

    default:
        break;
    }
}

static C9aux *srv(const char *s, struct vfs_driver_ctx_t *ctx) {
    int f;
    C9aux *c;

    c = &ctx->aux;

    if ((f = dial(s)) < 0)
        return NULL;

    c->f = f;
    c->c.read = ctxread;
    c->c.begin = ctxbegin;
    c->c.end = ctxend;
    c->c.error = ctxerror;
    c->c.aux = c;

    return c;
}

static C9aux *start(vfs_driver_ctx_t ctx) {
    C9aux *a = NULL;
    C9tag tag;
    int i;

    for (i = 0; i < 10; i++) {
        if ((a = srv(ctx->conf, ctx)) != NULL) {
            a->c.r = ctxprocR;
            c9version(&a->c, &tag, MSIZE);
            wrsend(a);
            return a;
        }
        sleep(1);
    }

    if (a == NULL)
        return NULL;

    return a;
}

static int proc(C9aux *a) {
    struct timeval t;
    fd_set r, e;
    C9ctx *ctx;

    FD_ZERO(&r);
    FD_SET(a->f, &r);
    FD_ZERO(&e);
    FD_SET(a->f, &e);
    memset(&t, 0, sizeof(t));
    t.tv_sec = 10;
    for (;;) {
        errno = 0;
        if (select(a->f + 1, &r, NULL, &e, &t) < 0 || FD_ISSET(a->f, &e)) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        break;
    }

    ctx = &a->c;
    if (FD_ISSET(a->f, &r)) {
        c9proc(ctx);
    } else {
        // const char *path[] = {NULL};
        // c9walk(ctx, &tag, 0, 0, path);
    }

    return 0;
}

vfs_driver_t *Vfs9PInit(const wapp_t *wapp, const char *opt) {
    vfs_driver_t *driver;
    (void)wapp;

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }

    memset(driver->ctx, 0, sizeof(struct vfs_driver_ctx_t));

    driver->ctx->conf = (char *)WantedMalloc(strlen(opt) + 1);
    if (NULL == driver->ctx->conf) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver->ctx);
        WantedFree(driver);
        return NULL;
    }

    memcpy(driver->ctx->conf, opt, strlen(opt) + 1);

    driver->bytesId = *(uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->OpenAt = _OpenAt;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Seek = _Seek;
    driver->ReadDir = _ReadDir;
    driver->Unlink = _Unlink;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    DEBUG_TRACE("9p Destroy");

    C9aux *a = &d->ctx->aux;

    if (a->flags & CONN_ATTACHED) {
        c9clunk(&a->c, &a->tag, 0);
        wrsend(a);

        close(a->f);
    }

    WantedFree(d->ctx->conf);
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    C9aux *a = &d->aux;

    const char *p[C9maxpathel + 1] = {NULL};
    char buf[strlen(path) + 1];
    int newFd;

    memcpy(buf, path, strlen(path) + 1);

    if (splitPath(buf, p) < 0)
        return -ENAMETOOLONG;

    // version/auth/attach
    DEBUG_TRACE("9p Open: %s", path);

    if (!(a->flags & CONN_ATTACHED)) {
        if (start(d) == NULL) {
            return -EAGAIN;
        }

        while (proc(a) == 0 && wrsend(a) == 0) {
            if (a->flags & CONN_ATTACHED)
                break;
        }

        d->fildes[0].opened = 0;
    }

    if (!d->fildes[0].opened) {
        c9open(&a->c, &a->tag, 0, C9read);
        wrsend(a);
        proc(a);

        d->fildes[0].opened = 1;
        d->fildes[0].currOff = 0;
        d->fildes[0].type = convert9pFiletype(a->lastQid.type);
    }

    if (memcmp(path, "/", 2) == 0) {
        return 0;
    }

    newFd = findFirstClosedFd(d);
    if (newFd < 0)
        return newFd;

    if (c9walk(&a->c, &a->tag, 0, newFd, p) != 0 || wrsend(a) != 0 ||
        proc(a) != 0)
        return -EIO;

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    C9mode mode;
    if (flags & VFS_O_RDWR) {
        mode = C9rdwr;
    } else if (flags & VFS_O_WRONLY) {
        mode = C9write;
    } else {
        mode = C9read;
    }

    if (c9open(&a->c, &a->tag, newFd, mode) != 0 || wrsend(a) != 0 ||
        proc(a) != 0)
        return -EIO;

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    d->fildes[newFd].opened = 1;
    d->fildes[newFd].currOff = 0;
    d->fildes[newFd].type = convert9pFiletype(a->lastQid.type);

    return newFd;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    (void)d;
    (void)flags;
    (void)fd;
    (void)path;
    /*
     * The VFS core dispatches OpenAt only to PLATFORM-type parent fds; a 9P
     * mount resolves relative paths through the router, which calls Open. This
     * slot is therefore never reached — reject rather than fake success.
     */
    return -ENOTSUP;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    C9aux *a = &d->aux;
    // close
    DEBUG_TRACE("9p Close: %d", fd);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    if (fd != 0) {
        c9clunk(&a->c, &a->tag, fd);
        wrsend(a);
        proc(a);
        d->fildes[fd].opened = 0;
    }

    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    C9stat *s;
    C9aux *a = &d->aux;

    // stat
    DEBUG_TRACE("9p Stat: %d", fd);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    a->haveStat = 0;
    if (c9stat(&a->c, &a->tag, fd) != 0 || wrsend(a) != 0 || proc(a) != 0)
        return -EIO;

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    if (!a->haveStat)
        return -EIO;
    s = &a->lastStat;

    stat->dev = *(uint32_t *)(id);
    stat->ino = s->qid.path;
    stat->filetype = convert9pFiletype(s->qid.type);
    ;
    stat->nlink = 0;
    stat->size = (uint32_t)s->size;
    stat->atim = s->atime;
    stat->ctim = 0;
    stat->mtim = s->mtime;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    C9aux *a = &d->aux;
    size_t r;
    const uint8_t *b;

    // read
    DEBUG_TRACE("9p Read: %d, %zu", fd, nbyte);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    if (d->fildes[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    c9read(&a->c, &a->tag, fd, d->fildes[fd].currOff, nbyte);
    wrsend(a);
    proc(a);

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    r = a->rCnt;
    b = &a->rBuf[RREAD_HDR_LEN];

    memcpy(buf, b, r);

    d->fildes[fd].currOff += r;

    return r;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    C9aux *a = &d->aux;

    // write
    DEBUG_TRACE("9p Write: %d, %zu", fd, nbyte);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    if (d->fildes[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    c9write(&a->c, &a->tag, fd, d->fildes[fd].currOff, buf, nbyte);
    wrsend(a);
    proc(a);

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    d->fildes[fd].currOff += nbyte;

    return nbyte;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    DEBUG_TRACE("9p Seek: %d, %ld (%u)", fd, off, whence);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    switch (whence) {
    case VFS_SEEK_SET:
        d->fildes[fd].currOff = off;
        break;
    case VFS_SEEK_CUR:
        d->fildes[fd].currOff += off;
        break;
    case VFS_SEEK_END:
        break;
    default:
        return -EINVAL;
    }

    *pos = d->fildes[fd].currOff;

    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    C9aux *a = &d->aux;
    C9stat s = {0};
    uint8_t *b;
    uint32_t sz;
    size_t used = 0;
    uint64_t off = 0;
    vfs_dirent_t dir = {0};

    // read and parse dir entry
    DEBUG_TRACE("9p ReadDir: %d, %zu", fd, bufLen);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    do {
        c9read(&a->c, &a->tag, fd, off, MSIZE - RREAD_HDR_LEN);
        wrsend(a);
        proc(a);
        if (a->rCnt < 7)
            break;
        off += a->rCnt;
        b = &a->rBuf[RREAD_HDR_LEN];
        sz = a->rCnt;

        while (sz > 0 && c9parsedir(&a->c, &s, &b, &sz) == 0) {
            dir.d_ino = s.qid.path;
            dir.d_type = convert9pFiletype(s.qid.type);
            dir.d_namlen = strnlen(s.name, MAX_PATH_LEN);
            dir.d_next = off;

            if (used + sizeof(dir) + dir.d_namlen > bufLen) {
                used = bufLen;
                break;
            }
            memcpy((char *)buf + used, &dir, sizeof(dir));
            memcpy((char *)buf + sizeof(dir) + used, s.name, dir.d_namlen);

            used += sizeof(dir) + dir.d_namlen;
        }
    } while (a->rCnt != 0);

    *bufUsed = used;
    *cookie = dir.d_next; // last found directory entry

    return 0;
}

static int _Unlink(vfs_driver_ctx_t d, int fd, const char *path) {
    C9aux *a = &d->aux;

    DEBUG_TRACE("9p Unlink: %d, %s", fd, path);

    if (fd >= MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    const char *p[C9maxpathel + 1] = {NULL};
    char buf[strlen(path) + 1];
    int newFd = fd;

    memcpy(buf, path, strlen(path) + 1);

    if (splitPath(buf, p) < 0)
        return -ENAMETOOLONG;

    c9walk(&a->c, &a->tag, 0, newFd, p);
    wrsend(a);
    proc(a);

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    c9remove(&a->c, &a->tag, newFd);
    wrsend(a);
    proc(a);

    if (a->flags & CONN_ERROR) {
        a->flags &= ~CONN_ERROR;
        return -EIO;
    }

    return 0;
}
