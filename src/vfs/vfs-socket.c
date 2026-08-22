/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <wanted_log.h>
#include <wanted_malloc.h>

#include <platform.h>

/* Socket address buffer length, in bytes, per connection context. */
#define MAX_ADDR_LEN 32

/* Longest transport spec the option string may carry: scheme, "://", host and
 * port. */
#define MAX_SPEC_LEN (MAX_ADDR_LEN + 16)

/* Connections a listener may hold open at once, on top of the slot the
 * configured socket itself occupies. A build without the listen role has the
 * one slot and no accept path at all. */
#ifdef CONFIG_WANTED_VFS_SOCKET_LISTEN
#define SOCK_MAX_CONNS CONFIG_WANTED_VFS_SOCKET_MAX_CONNS
#else
#define SOCK_MAX_CONNS 0
#endif
#define SOCK_MAX_SLOTS (1 + SOCK_MAX_CONNS)

/* The slot the configured socket occupies: the outbound connection, or the
 * listener that accepted connections descend from. */
#define SOCK_SELF 0

#define DEFAULT_BACKLOG 4

static const char id[] = {'S', 'o', 'c', 'k'};

/* One transport endpoint. A connect-role driver uses the SOCK_SELF slot alone;
 * a listener holds its bound socket there and one accepted connection per
 * further slot, each addressed by the fd the accept handed back. */
struct sock_conn_t {
    void *netCtx;
    bool inUse;
    bool connected;
};

struct vfs_driver_ctx_t {
    /* Watched beside the socket so a stop ends a blocking wait; -1 when the
     * platform interrupts by signal. */
    int wakeFd;
    uint8_t type;
    const char addr[MAX_ADDR_LEN];
    uint16_t port;
    vfs_oflags_t flags;
    bool listening;
    uint16_t backlog;
    uint8_t maxConns;
    struct sock_conn_t conns[SOCK_MAX_SLOTS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t c, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t c, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t c, int fd);
static int _Read(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte);
static int _Stat(vfs_driver_ctx_t c, int fd, vfs_stat_t *stat);
static int _SockAccept(vfs_driver_ctx_t c, int fd, vfs_oflags_t flags,
                       int *newFd);
static void _SetWake(vfs_driver_ctx_t c, int fd);
static int _SockRecv(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte,
                     vfs_riflags_t iflags, vfs_roflags_t *oflags);
static int _SockSend(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte,
                     vfs_sdflags_t flags);
static int _SockShutdown(vfs_driver_ctx_t c, int fd, vfs_sdflags_t flags);

static vfs_filetype_t convertSocketType(uint8_t type) {
    switch (type) {
    case VFS_SKT_TCP:
    case VFS_SKT_STCP:
        return VFS_FILETYPE_SOCKET_STREAM;
    case VFS_SKT_UDP:
    case VFS_SKT_SUDP:
        return VFS_FILETYPE_SOCKET_DGRAM;
    case VFS_SKT_SERIAL:
        return VFS_FILETYPE_CHARACTER_DEVICE;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

static bool isStream(uint8_t type) {
    return type == VFS_SKT_TCP || type == VFS_SKT_STCP;
}

static bool isSecure(uint8_t type) {
    return type == VFS_SKT_STCP || type == VFS_SKT_SUDP;
}

/* Map a URL scheme to a socket transport type. Returns false for an unknown
 * scheme. The "s" suffix selects the secured transport (TLS / DTLS). */
static bool schemeToType(const char *scheme, size_t len, uint8_t *type) {
    if (len == 3 && strncmp(scheme, "tcp", 3) == 0)
        *type = VFS_SKT_TCP;
    else if (len == 4 && strncmp(scheme, "tcps", 4) == 0)
        *type = VFS_SKT_STCP;
    else if (len == 3 && strncmp(scheme, "udp", 3) == 0)
        *type = VFS_SKT_UDP;
    else if (len == 4 && strncmp(scheme, "udps", 4) == 0)
        *type = VFS_SKT_SUDP;
    else if (len == 6 && strncmp(scheme, "serial", 6) == 0)
        *type = VFS_SKT_SERIAL;
    else
        return false;
    return true;
}

/* Read a bounded decimal parameter value. Returns false on anything that is
 * not a whole number within [1, max]. */
static bool parseNum(const char *val, size_t len, long max, long *out) {
    long v = 0;
    if (len == 0 || len > 5)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (val[i] < '0' || val[i] > '9')
            return false;
        v = v * 10 + (val[i] - '0');
    }
    if (v < 1 || v > max)
        return false;
    *out = v;
    return true;
}

/* Parse the ";key=value" parameters trailing the transport spec: the role,
 * backlog and max_conns the sockets[] installer appends to the address.
 * Returns false on an unknown key or unusable value; the caller rejects it. */
static bool parseParams(const char *p, bool *listening, uint16_t *backlog,
                        uint8_t *maxConns, bool *hasBacklog,
                        bool *hasMaxConns) {
    while (*p == ';') {
        const char *key = p + 1;
        const char *eq = strchr(key, '=');
        if (eq == NULL)
            return false;
        size_t keyLen = (size_t)(eq - key);
        const char *val = eq + 1;
        const char *end = strchr(val, ';');
        size_t valLen = end ? (size_t)(end - val) : strlen(val);
        long num;

        if (keyLen == 4 && strncmp(key, "role", 4) == 0) {
            if (valLen == 6 && strncmp(val, "listen", 6) == 0)
                *listening = true;
            else if (valLen == 7 && strncmp(val, "connect", 7) == 0)
                *listening = false;
            else
                return false;
        } else if (keyLen == 7 && strncmp(key, "backlog", 7) == 0) {
            if (!parseNum(val, valLen, 64, &num))
                return false;
            *backlog = (uint16_t)num;
            *hasBacklog = true;
        } else if (keyLen == 9 && strncmp(key, "max_conns", 9) == 0) {
            if (!parseNum(val, valLen, SOCK_MAX_CONNS ? SOCK_MAX_CONNS : 1,
                          &num))
                return false;
            *maxConns = (uint8_t)num;
            *hasMaxConns = true;
        } else {
            return false;
        }

        p = end ? end : val + valLen;
    }
    return *p == '\0';
}

vfs_driver_t *VfsSocketInit(const wapp_t *wapp, const char *options) {
    uint16_t port;
    vfs_driver_t *driver;
    char addr[MAX_ADDR_LEN];
    char spec[MAX_SPEC_LEN];
    bool listening = false;
    bool hasBacklog = false;
    bool hasMaxConns = false;
    uint16_t backlog = DEFAULT_BACKLOG;
    uint8_t maxConns = SOCK_MAX_CONNS ? SOCK_MAX_CONNS : 1;

    (void)wapp;

    if (NULL == options) {
        DEBUG_TRACE("bad options");
        return NULL;
    }

    /* The entry's address is a URL "<scheme>://<host>:<port>"; the scheme picks
     * the transport (tcp, tcps, udp, udps, serial). The launch config's
     * remaining socket fields arrive as ";key=value" parameters behind it. */
    const char *params = strchr(options, ';');
    size_t specLen = params ? (size_t)(params - options) : strlen(options);
    if (specLen >= sizeof(spec)) {
        DEBUG_TRACE("socket address: too long");
        return NULL;
    }
    memcpy(spec, options, specLen);
    spec[specLen] = '\0';

    if (params != NULL && !parseParams(params, &listening, &backlog, &maxConns,
                                       &hasBacklog, &hasMaxConns)) {
        LOG_ERROR("socket '%s': bad role/backlog/max_conns", spec);
        return NULL;
    }

    const char *sep = strstr(spec, "://");
    if (NULL == sep) {
        DEBUG_TRACE("socket address: missing scheme");
        return NULL;
    }

    uint8_t type;
    if (!schemeToType(spec, (size_t)(sep - spec), &type)) {
        DEBUG_TRACE("socket address: unknown scheme");
        return NULL;
    }

    if (!SECURE_SOCKETS && isSecure(type)) {
        DEBUG_TRACE("no support for secure sockets");
        return NULL;
    }

    if (listening) {
#ifndef CONFIG_WANTED_VFS_SOCKET_LISTEN
        LOG_ERROR("socket '%s': the listen role is not compiled in", spec);
        return NULL;
#else
        if (type == VFS_SKT_SERIAL) {
            LOG_ERROR("socket '%s': a serial device cannot listen", spec);
            return NULL;
        }
        if (isSecure(type)) {
            /* Accept-side TLS needs a server certificate and key, and nothing
             * supplies them. */
            LOG_ERROR("socket '%s': a secure transport cannot listen", spec);
            return NULL;
        }
        if (!isStream(type) && (hasBacklog || hasMaxConns)) {
            LOG_ERROR("socket '%s': backlog/max_conns need a stream transport",
                      spec);
            return NULL;
        }
#endif
    } else if (hasBacklog || hasMaxConns) {
        LOG_ERROR("socket '%s': backlog/max_conns need the listen role", spec);
        return NULL;
    }

    const char *host = sep + 3;
    if (type == VFS_SKT_SERIAL) {
        /* "serial:///dev/ttyACM0" - a bare device path, no port to parse. */
        size_t hostLen = strlen(host);
        if (hostLen == 0 || hostLen >= MAX_ADDR_LEN) {
            DEBUG_TRACE("socket address: bad device path");
            return NULL;
        }
        memcpy(addr, host, hostLen);
        addr[hostLen] = '\0';
        port = 0;
    } else {
        /* Host runs from after "://" up to the ':' that introduces the port.
         */
        const char *colon = strrchr(host, ':');
        if (NULL == colon || colon == host) {
            DEBUG_TRACE("socket address: missing host or port");
            return NULL;
        }
        size_t hostLen = (size_t)(colon - host);
        if (hostLen >= MAX_ADDR_LEN) {
            DEBUG_TRACE("socket address: host too long");
            return NULL;
        }

        char *endp = NULL;
        long portVal = strtol(colon + 1, &endp, 10);
        if (endp == colon + 1 || *endp != '\0' || portVal <= 0 ||
            portVal > 65535) {
            DEBUG_TRACE("socket address: bad port");
            return NULL;
        }
        port = (uint16_t)portVal;

        memcpy(addr, host, hostLen);
        addr[hostLen] = '\0';
    }

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }
    /* Zero first: an unassigned vtable slot would otherwise hold heap
     * garbage, which the caller reads as a function to call. */
    memset(driver, 0, sizeof(*driver));

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(driver->ctx, 0, sizeof(struct vfs_driver_ctx_t));
    driver->ctx->wakeFd = -1; /* 0 is a valid descriptor */

    driver->bytesId = *(uint32_t *)(id);
    driver->filetype = convertSocketType(type);
    driver->ctx->type = type;
    strncpy((char *)driver->ctx->addr, addr, MAX_ADDR_LEN);
    driver->ctx->port = port;
    driver->ctx->listening = listening;
    driver->ctx->backlog = backlog;
    driver->ctx->maxConns = maxConns;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->OpenAt = _OpenAt;
    driver->Close = _Close;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Stat = _Stat;
    driver->SockAccept = _SockAccept;
    driver->SetWake = _SetWake;
    driver->SockRecv = _SockRecv;
    driver->SockSend = _SockSend;
    driver->SockShutdown = _SockShutdown;

    return driver;
}

/* The slot an fd addresses, or NULL when the fd names no live connection. */
static struct sock_conn_t *conn(vfs_driver_ctx_t c, int fd) {
    if (fd < 0 || fd >= SOCK_MAX_SLOTS)
        return NULL;
    if (!c->conns[fd].inUse)
        return NULL;
    return &c->conns[fd];
}

static void releaseConn(struct sock_conn_t *s) {
    if (s->netCtx != NULL) {
        PlatformNetClose(s->netCtx);
        PlatformNetFree(s->netCtx);
    }
    s->netCtx = NULL;
    s->inUse = false;
    s->connected = false;
}

static int _Destroy(struct vfs_driver_t *d) {
    for (int i = 0; i < SOCK_MAX_SLOTS; i++) {
        if (d->ctx->conns[i].inUse)
            releaseConn(&d->ctx->conns[i]);
    }
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int _Open(vfs_driver_ctx_t c, const char *path, vfs_oflags_t flags) {
    struct sock_conn_t *s = &c->conns[SOCK_SELF];
    (void)path;

    if (s->connected) {
        return SOCK_SELF;
    }

    if (s->inUse) {
        releaseConn(s);
    }

    s->netCtx = PlatformNetOpen(c->type);
    if (s->netCtx == NULL) {
        return -ECONNABORTED;
    }
    s->inUse = true;
    c->flags = flags;

    if (c->listening) {
        int ret = PlatformNetListen(s->netCtx, c->addr, c->port, c->backlog);
        if (ret < 0) {
            releaseConn(s);
            LOG_ERROR("socket: can't bind %s:%u (%d)", c->addr, c->port, ret);
            return ret;
        }
        /* A bound socket is ready to serve; nothing is left to establish. */
        s->connected = true;
    }

    return SOCK_SELF;
}

static int _OpenAt(vfs_driver_ctx_t c, int fd, const char *path,
                   vfs_oflags_t flags) {
    (void)fd;
    return _Open(c, path, flags);
}

static int _Close(vfs_driver_ctx_t c, int fd) {
    struct sock_conn_t *s = conn(c, fd);
    if (s == NULL)
        return -EBADF;

    if (fd == SOCK_SELF) {
        /* The configured socket outlives a close: reopening it reconnects (or
         * rebinds) through the same driver instance. */
        s->connected = false;
        return PlatformNetClose(s->netCtx);
    }

    releaseConn(s);
    return 0;
}

/* Establish an outbound connection lazily on the first I/O. */
static int ensureConnected(vfs_driver_ctx_t c, struct sock_conn_t *s) {
    if (s->connected)
        return 0;
    int ret = PlatformNetConnect(s->netCtx, c->addr, c->port);
    if (ret < 0)
        return ret;
    s->connected = true;
    return 0;
}

/* Resolve an fd to a slot ready for payload I/O. A stream listener carries no
 * payload of its own — only the connections accepted from it do. */
static int ioConn(vfs_driver_ctx_t c, int fd, struct sock_conn_t **out) {
    struct sock_conn_t *s = conn(c, fd);
    if (s == NULL)
        return -EBADF;
    if (fd == SOCK_SELF && c->listening && isStream(c->type))
        return -ENOTCONN;
    int ret = ensureConnected(c, s);
    if (ret < 0)
        return ret;
    *out = s;
    return 0;
}

/* A wapp cannot subscribe an fd to poll_oneoff, thus a non-blocking read is
 * the only way one waits on a socket and does anything else. */
static int _Read(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte) {
    struct sock_conn_t *s;
    int ret = ioConn(c, fd, &s);
    if (ret < 0)
        return ret;
    ret = PlatformNetWaitReadable(s->netCtx, c->wakeFd,
                                  (c->flags & VFS_O_NONBLOCK) ? 0 : -1);
    if (ret < 0)
        return ret;
    return PlatformNetRecv(s->netCtx, buf, nbyte, 0);
}

static void _SetWake(vfs_driver_ctx_t c, int fd) {
    if (c != NULL)
        c->wakeFd = fd;
}

static int _Write(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte) {
    struct sock_conn_t *s;
    int ret = ioConn(c, fd, &s);
    if (ret < 0)
        return ret;
    return PlatformNetSend(s->netCtx, buf, nbyte, 0);
}

static int _Stat(vfs_driver_ctx_t c, int fd, vfs_stat_t *stat) {
    if (NULL == stat)
        return -EINVAL;

    const struct sock_conn_t *s = conn(c, fd);

    stat->dev = *(uint32_t *)id;
    stat->ino = c->port;
    stat->filetype = convertSocketType(c->type);
    stat->size = s != NULL && s->connected;
    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->oflags = c->flags;

    return 0;
}

/* `newFd` is an out-parameter of the driver vtable's SockAccept slot; a build
 * without the listen role writes through it nowhere, which reads as const to
 * the analysers. */
static int _SockAccept(vfs_driver_ctx_t c, int fd, vfs_oflags_t flags,
                       /* cppcheck-suppress constParameterCallback */
                       /* NOLINTNEXTLINE(readability-non-const-parameter) */
                       int *newFd) {
    (void)flags;

    if (newFd == NULL) {
        return -EINVAL;
    }

#ifndef CONFIG_WANTED_VFS_SOCKET_LISTEN
    (void)c;
    (void)fd;
    return -ENOTSUP;
#else
    struct sock_conn_t *l = conn(c, fd);
    if (l == NULL)
        return -EBADF;
    if (fd != SOCK_SELF || !c->listening || !isStream(c->type))
        return -ENOTSUP;

    int slot = -1;
    int live = 0;
    for (int i = SOCK_SELF + 1; i < SOCK_MAX_SLOTS; i++) {
        if (c->conns[i].inUse)
            live++;
        else if (slot < 0)
            slot = i;
    }
    if (slot < 0 || live >= c->maxConns)
        return -ENFILE;

    /* Wait first, watching the wapp's wake descriptor beside the listener, so
     * a stop ends the wait. Without one the accept blocks and a signal ends
     * it. */
    int ret = PlatformNetWaitReadable(l->netCtx, c->wakeFd, -1);
    if (ret < 0)
        return ret;

    struct netCtx *accepted = NULL;
    ret = PlatformNetAccept(l->netCtx, &accepted);
    if (ret < 0)
        return ret;

    c->conns[slot].netCtx = accepted;
    c->conns[slot].inUse = true;
    /* An accepted connection is established the moment it exists. */
    c->conns[slot].connected = true;

    *newFd = slot;
    return 0;
#endif
}

static int _SockRecv(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte,
                     vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    struct sock_conn_t *s;
    if (oflags != NULL)
        *oflags = 0;
    int ret = ioConn(c, fd, &s);
    if (ret < 0)
        return ret;
    if (c->flags & VFS_O_NONBLOCK) {
        ret = PlatformNetWaitReadable(s->netCtx, c->wakeFd, 0);
        if (ret < 0)
            return ret;
    }
    return PlatformNetRecv(s->netCtx, buf, nbyte, iflags);
}

static int _SockSend(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte,
                     vfs_sdflags_t flags) {
    struct sock_conn_t *s;
    int ret = ioConn(c, fd, &s);
    if (ret < 0)
        return ret;
    return PlatformNetSend(s->netCtx, buf, nbyte, flags);
}

static int _SockShutdown(vfs_driver_ctx_t c, int fd, vfs_sdflags_t flags) {
    struct sock_conn_t *s = conn(c, fd);
    if (s == NULL)
        return -EBADF;

    return PlatformNetShutdown(s->netCtx, flags);
}
