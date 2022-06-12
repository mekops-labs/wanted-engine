#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include <vfs.h>
#include <vfs-drivers.h>

#include <debug_trace.h>
#include <wanted_malloc.h>

#include <c9.h>


static const char id[] = { 'S', 't', 'y', 'x' };

static int _Destroy (struct vfs_driver_t *d);
static int _Open    (vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt  (vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close   (vfs_driver_ctx_t d, int fd);
static int _Stat    (vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read    (vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write   (vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek    (vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos);
static int _ReadDir (vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed);

#define MSIZE 8192
#define ERROR    1<<0
#define ATTACHED   1<<1
#define DISCONNECTED     1<<2
#define ADDRESS "tcp!localhost!5640"

typedef struct C9aux {
    int f;
    int flags;
    uint8_t rBuf[MSIZE];
    uint8_t wBuf[MSIZE];
    size_t  wOff;
    C9ctx c;
    C9tag tag;
} C9aux;

struct vfs_driver_ctx_t {
    C9aux aux;
};

static int
wrsend(C9aux *a)
{
	uint32_t n;
	int w;

	for (n = 0; n < a->wOff; n += w) {
		if ((w = write(a->f, a->wBuf+n, a->wOff-n)) <= 0) {
			if (errno == EINTR)
				continue;
			if (errno != EPIPE) /* remote end closed */
				perror("write");
			return -1;
		}
	}
	a->wOff = 0;

	return 0;
}

static uint8_t *
ctxbegin(C9ctx *ctx, uint32_t size)
{
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

static int
ctxend(C9ctx *ctx)
{
	(void)ctx;
	return 0;
}

static uint8_t *
ctxread(C9ctx *ctx, uint32_t size, int *err)
{
	uint32_t n;
	int r;
	C9aux *a;

	a = ctx->aux;
	*err = 0;
	for (n = 0; n < size; n += r) {
		if ((r = read(a->f, a->rBuf+n, size-n)) <= 0) {
			if (errno == EINTR)
				continue;
			a->flags |= DISCONNECTED;
			close(a->f);
			return NULL;
		}
	}

	return a->rBuf;
}

__attribute__ ((format (printf, 1, 2)))
static void
ctxerror(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	fprintf(stderr, "\n");
	va_end(ap);
}

static int
dial(char *s)
{
	struct addrinfo *r, *a, hint = {.ai_flags = AI_ADDRCONFIG, .ai_family = AF_UNSPEC, 0};
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
	if ((port = strchr(s+4, '!')) == NULL) {
		DEBUG_TRACE("invalid dial string: %s", s);
		return -1;
	}
	if (snprintf(host, sizeof(host), "%.*s", (int)(port-s-4), s+4) >= (int)sizeof(host)) {
		DEBUG_TRACE("host name too large: %s", s);
		return -1;
	}
	port++;
	if ((e = getaddrinfo(host, port, &hint, &r)) != 0){
		DEBUG_TRACE("%s: %s", gai_strerror(e), s);
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

static void
ctxprocR(C9ctx *ctx, C9r *r)
{
	C9aux *a;
	C9tag tag;
	const char *path[2];
	char buf[64];

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
        DEBUG_TRACE("Rread");
		// if (chatoff >= skipuntil)
		// 	output(r->read.data, r->read.size);
		// chatoff += r->read.size;
		/* fallthrough */
        break;
	case Ropen:
        DEBUG_TRACE("Ropen");
		// if ((a->flags & Joined) == 0 && printjoin) {
		// 	c9write(ctx, &tag, Chatfid, 0, buf, snprintf(buf, sizeof(buf), "JOIN %s to chat\n", nick));
		// 	a->flags |= Joined;
		// }
		// c9read(ctx, &tag, Chatfid, chatoff, chatoff < skipuntil ? skipuntil-chatoff : Msize);
		break;

	case Rerror:
		DEBUG_TRACE("Rerror: %s", r->error);
		a->flags = ERROR;
		break;

	default:
		break;
	}
}

static C9aux *
srv(char *s, struct vfs_driver_ctx_t *ctx)
{
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

static C9aux *
start(vfs_driver_ctx_t ctx)
{
	C9aux *a;
	C9tag tag;
	int i;

	for (i = 0; i < 10; i++) {
        if ((a = srv(ADDRESS, ctx)) != NULL) {
            a->c.r = ctxprocR;
            c9version(&a->c, &tag, MSIZE);
            wrsend(a);
            return a;
        }
		sleep(10);
	}

	if (a == NULL)
		return NULL;

	return a;
}

static int
proc(C9aux *a)
{
	struct timeval t;
	int n, sz, sz0;
	fd_set r, e;
	C9tag tag;
	C9ctx *ctx;
	char *s;

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



vfs_driver_t *Vfs9PInit(const wapp_t *wapp, uint8_t argc, const char *args[]) {
    // Todo:
    // 1. Create context and buffers
    // 2. connect comm backend

    vfs_driver_t *driver;

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = VFS_FILETYPE_DIRECTORY;
    driver->Destroy         = _Destroy;
    driver->Open            = _Open;
    driver->OpenAt          = _OpenAt;
    driver->Close           = _Close;
    driver->Stat            = _Stat;
    driver->Read            = _Read;
    driver->Write           = _Write;
    driver->Seek            = _Seek;
    driver->ReadDir         = _ReadDir;

    return driver;
}

static int _Destroy (struct vfs_driver_t *d)
{
    DEBUG_TRACE("9p Destroy");

    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    // version/auth/attach
    DEBUG_TRACE("9p Open: %s", path);

    start(d);

    while (proc(&d->aux) == 0 && wrsend(&d->aux) == 0) {
        if (d->aux.flags &= ATTACHED) {
            break;
        }
    };

    return 0;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    // walk
    DEBUG_TRACE("9p OpenAt: %d, %s", fd, path);
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    // close
    DEBUG_TRACE("9p Close: %d", fd);

/*
    if (fd == 0) {
        c9clunk(&d->aux.c, &d->aux.tag, 0);
        wrsend(&d->aux);
    }
    */
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    // stat
    DEBUG_TRACE("9p Stat: %d", fd);

    stat->dev = *(uint32_t*)(id);
    stat->ino = 0;
    stat->filetype = VFS_FILETYPE_DIRECTORY;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    // read
    DEBUG_TRACE("9p Read: %d, %zu", fd, nbyte);

    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    // write
    DEBUG_TRACE("9p Write: %d, %zu", fd, nbyte);

    return nbyte;
}


static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence, long *pos)
{
    DEBUG_TRACE("9p Seek: %d, %ld (%u)", fd, off, whence);

    return 0;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen, uint64_t *cookie, size_t *bufUsed)
{
    // read and parse dir entry
    DEBUG_TRACE("9p ReadDir: %d, %zu", fd, bufLen);

    return 0;
}