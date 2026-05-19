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

#define MSIZE 8192u
#define ERROR 0x1
#define ATTACHED 0x2
#define DISCONNECTED 0x4

// TODO: make configurable
#define MAX_OPENED_FILES 10

typedef struct C9aux {
    int f;
    int flags;
    uint8_t rBuf[MSIZE];
    size_t rCnt;
    uint8_t wBuf[MSIZE];
    size_t wOff;
    C9stat *lastStat;
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

static int FindFirstClosedFd(vfs_driver_ctx_t d) {
    if (!d)
        return -EINVAL;

    for (int i = 0; i < MAX_OPENED_FILES; i++) {
        if (!d->fildes[i].opened) {
            return i;
        }
    }
    return -EMFILE;
}

static vfs_filetype_t convert9pFiletype(C9qt t) {
    if (t & C9qtdir) {
        return VFS_FILETYPE_DIRECTORY;
    } else if (t & C9qtfile) {
        return VFS_FILETYPE_CHARACTER_DEVICE;
    } else {
        return VFS_FILETYPE_REGULAR_FILE;
    }

    return VFS_FILETYPE_UNKNOWN;
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
                a->flags &= ~ATTACHED;
                close(a->f);
            }
            return -1;
        }
    }
    a->wOff = 0;

    return 0;
}

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
                a->flags &= ~ATTACHED;
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

static int dial(char *s) {
    struct addrinfo *r, *a,
        hint = {.ai_flags = AI_ADDRCONFIG, .ai_family = AF_UNSPEC, 0};
    char host[64], *port;
    int e, f;

    if (strncmp(s, "udp!", 4) == 0) {
        hint.ai_socktype = SOCK_DGRAM;
        hint.ai_protocol = IPPROTO_UDP;
    } else if (strncmp(s, "tcp!", 4) == 0) {
        hint.ai_socktype = SOCK_STREAM;
        hint.ai_protocol = IPPROTO_TCP;
    } else {
        DEBUG_TRACE("invalid dial string: %s", s);
        return -1;
    }
    if ((port = strchr(s + 4, '!')) == NULL) {
        DEBUG_TRACE("invalid dial string: %s", s);
        return -1;
    }
    if (snprintf(host, sizeof(host), "%.*s", (int)(port - s - 4), s + 4) >=
        (int)sizeof(host)) {
        DEBUG_TRACE("host name too large: %s", s);
        return -1;
    }
    port++;
    if ((e = getaddrinfo(host, port, &hint, &r)) != 0) {
        // DEBUG_TRACE("%s: %s", gai_strerror(e), s);
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
        a->flags = ATTACHED;
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
        a->lastStat = &r->stat;
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
        a->flags = ERROR;
        break;

    default:
        break;
    }
}

static C9aux *srv(char *s, struct vfs_driver_ctx_t *ctx) {
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
    C9aux *a;
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
    // Todo:
    // 1. Create context and buffers
    // 2. connect comm backend

    vfs_driver_t *driver;

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

    if (a->flags & ATTACHED) {
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

    // TODO: max depth is 3 currently - dirty!
    const char *p[3] = {NULL};
    char buf[strlen(path) + 1];
    int newFd;

    memcpy(buf, path, strlen(path));
    buf[strlen(path)] = '\0';

    int i = 0;
    p[i++] = strtok(buf, "/");
    while ((p[i++] = strtok(NULL, "/")) != NULL) {
    };

    // version/auth/attach
    DEBUG_TRACE("9p Open: %s", path);

    if (!(a->flags & ATTACHED)) {
        if (start(d) == NULL) {
            return -EAGAIN;
        }

        while (proc(a) == 0 && wrsend(a) == 0) {
            if (a->flags & ATTACHED)
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

    newFd = FindFirstClosedFd(d);
    if (newFd < 0)
        return newFd;

    // TODO: error handling
    c9walk(&a->c, &a->tag, 0, newFd, p);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
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

    c9open(&a->c, &a->tag, newFd, mode);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
        return -EIO;
    }

    d->fildes[newFd].opened = 1;
    d->fildes[newFd].currOff = 0;
    d->fildes[newFd].type = convert9pFiletype(a->lastQid.type);

    return newFd;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    // TODO: OpenAt seems not used in drivers
    DEBUG_TRACE("9p OpenAt: %d, %s", fd, path);
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    C9aux *a = &d->aux;
    // close
    DEBUG_TRACE("9p Close: %d", fd);

    if (fd > MAX_OPENED_FILES || fd < 0)
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

    if (fd > MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    c9stat(&a->c, &a->tag, fd);
    wrsend(a);
    proc(a);

    // TODO: error handling

    s = a->lastStat;

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
    uint8_t *b;

    // read
    DEBUG_TRACE("9p Read: %d, %zu", fd, nbyte);

    if (fd > MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    if (d->fildes[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    c9read(&a->c, &a->tag, fd, d->fildes[fd].currOff, nbyte);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
        return -EIO;
    }

    r = a->rCnt;
    b = &a->rBuf[7];

    memcpy(buf, b, r);

    d->fildes[fd].currOff += r;

    return r;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    C9aux *a = &d->aux;

    // write
    DEBUG_TRACE("9p Write: %d, %zu", fd, nbyte);

    if (fd > MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    if (d->fildes[fd].type == VFS_FILETYPE_DIRECTORY) {
        return -EISDIR;
    }

    c9write(&a->c, &a->tag, fd, d->fildes[fd].currOff, buf, nbyte);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
        return -EIO;
    }

    d->fildes[fd].currOff += nbyte;

    return nbyte;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    DEBUG_TRACE("9p Seek: %d, %ld (%u)", fd, off, whence);

    if (fd > MAX_OPENED_FILES || fd < 0)
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
    vfs_dirent_t dir;

    // read and parse dir entry
    DEBUG_TRACE("9p ReadDir: %d, %zu", fd, bufLen);

    if (fd > MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    do {
        c9read(&a->c, &a->tag, fd, off, MSIZE - 7);
        wrsend(a);
        proc(a);
        if (a->rCnt < 7)
            break;
        off += a->rCnt;
        b = &a->rBuf[7];
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
            memcpy(buf + used, &dir, sizeof(dir));
            memcpy(buf + sizeof(dir) + used, s.name, dir.d_namlen);

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

    if (fd > MAX_OPENED_FILES || fd < 0)
        return -EBADF;

    // TODO: max depth is 3 currently - dirty!
    const char *p[3] = {NULL};
    char buf[strlen(path) + 1];
    int newFd = fd;

    memcpy(buf, path, strlen(path));
    buf[strlen(path)] = '\0';

    int i = 0;
    p[i++] = strtok(buf, "/");
    while ((p[i++] = strtok(NULL, "/")) != NULL) {
    };

    c9walk(&a->c, &a->tag, 0, newFd, p);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
        return -EIO;
    }

    c9remove(&a->c, &a->tag, newFd);
    wrsend(a);
    proc(a);

    if (a->flags & ERROR) {
        a->flags &= ~ERROR;
        return -EIO;
    }

    return 0;
}
