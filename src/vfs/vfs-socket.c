/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>

#include <platform.h>

/* Socket address buffer length, in bytes, per connection context. */
#define MAX_ADDR_LEN 32

static const char id[] = {'S', 'o', 'c', 'k'};

/* One connection per driver instance: the context holds a single transport's
 * state, so a re-open replaces any prior connection. */
struct vfs_driver_ctx_t {
    uint8_t type;
    const char addr[MAX_ADDR_LEN];
    uint16_t port;
    vfs_oflags_t flags;
    bool connected;
    void *netCtx;
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
    case VFS_SKT_UART:
        return VFS_FILETYPE_CHARACTER_DEVICE;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
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
    else if (len == 4 && strncmp(scheme, "uart", 4) == 0)
        *type = VFS_SKT_UART;
    else
        return false;
    return true;
}

vfs_driver_t *VfsSocketInit(const wapp_t *wapp, const char *options) {
    uint16_t port;
    vfs_driver_t *driver;
    char addr[MAX_ADDR_LEN];

    (void)wapp;

    if (NULL == options) {
        DEBUG_TRACE("bad options");
        return NULL;
    }

    /* The address is a URL "<scheme>://<host>:<port>"; the scheme picks the
     * transport (tcp, tcps, udp, udps). */
    const char *sep = strstr(options, "://");
    if (NULL == sep) {
        DEBUG_TRACE("socket address: missing scheme");
        return NULL;
    }

    uint8_t type;
    if (!schemeToType(options, (size_t)(sep - options), &type)) {
        DEBUG_TRACE("socket address: unknown scheme");
        return NULL;
    }

    if (!SECURE_SOCKETS && (type == VFS_SKT_STCP || type == VFS_SKT_SUDP)) {
        DEBUG_TRACE("no support for secure sockets");
        return NULL;
    }

    const char *host = sep + 3;
    if (type == VFS_SKT_UART) {
        /* "uart:///dev/ttyS1" - a bare device path, no port to parse. */
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

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(driver->ctx, 0, sizeof(struct vfs_driver_ctx_t));

    driver->bytesId = *(uint32_t *)(id);
    driver->filetype = convertSocketType(type);
    driver->ctx->type = type;
    strncpy((char *)driver->ctx->addr, addr, MAX_ADDR_LEN);
    driver->ctx->port = port;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->OpenAt = _OpenAt;
    driver->Close = _Close;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Stat = _Stat;
    driver->SockAccept = _SockAccept;
    driver->SockRecv = _SockRecv;
    driver->SockSend = _SockSend;
    driver->SockShutdown = _SockShutdown;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    PlatformNetClose(d->ctx->netCtx);
    PlatformNetFree(d->ctx->netCtx);
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int _Open(vfs_driver_ctx_t c, const char *path, vfs_oflags_t flags) {
    (void)path;
    if (c->connected) {
        return 0;
    }

    if (c->netCtx) {
        // only single connection supported, so close and free old context
        PlatformNetClose(c->netCtx);
        PlatformNetFree(c->netCtx);
    }

    c->netCtx = PlatformNetOpen(c->type);
    if (c->netCtx) {
        c->connected = false;
        c->flags = flags;
    } else {
        return -ECONNABORTED;
    }

    return 0;
}

static int _OpenAt(vfs_driver_ctx_t c, int fd, const char *path,
                   vfs_oflags_t flags) {
    (void)fd;
    return _Open(c, path, flags);
}

static int _Close(vfs_driver_ctx_t c, int fd) {
    (void)fd;
    c->connected = false;
    return PlatformNetClose(c->netCtx);
}

/* Establish the connection lazily on the first I/O. */
static int ensureConnected(vfs_driver_ctx_t c) {
    if (c->connected)
        return 0;
    int ret = PlatformNetConnect(c->netCtx, c->addr, c->port);
    if (ret < 0)
        return ret;
    c->connected = true;
    return 0;
}

static int _Read(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte) {
    (void)fd;
    int ret = ensureConnected(c);
    if (ret < 0)
        return ret;
    return PlatformNetRecv(c->netCtx, buf, nbyte, 0);
}

static int _Write(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte) {
    (void)fd;
    int ret = ensureConnected(c);
    if (ret < 0)
        return ret;
    return PlatformNetSend(c->netCtx, buf, nbyte, 0);
}

static int _Stat(vfs_driver_ctx_t c, int fd, vfs_stat_t *stat) {
    (void)fd;
    if (NULL == stat)
        return -EINVAL;

    stat->dev = *(uint32_t *)id;
    stat->ino = c->port;
    stat->filetype = convertSocketType(c->type);
    stat->size = c->connected;
    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->oflags = c->flags;

    return 0;
}

static int _SockAccept(vfs_driver_ctx_t c, int fd, vfs_oflags_t flags,
                       int *newFd) {
    (void)fd;
    (void)flags;
    int ret;
    if (newFd == NULL) {
        return -EINVAL;
    }

    /*
     * The accepted connection is returned as a bare fd, but the driver tracks
     * a single netCtx, so subsequent I/O ops still target the listening
     * context rather than the accepted socket. Backing each accepted fd with
     * its own context is required to make them independently readable.
     */
    ret = PlatformNetAccept(c->netCtx);

    if (ret >= 0)
        *newFd = ret;

    return ret;
}

static int _SockRecv(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte,
                     vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    (void)fd;
    if (oflags != NULL)
        *oflags = 0;
    int ret = ensureConnected(c);
    if (ret < 0)
        return ret;
    return PlatformNetRecv(c->netCtx, buf, nbyte, iflags);
}

static int _SockSend(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte,
                     vfs_sdflags_t flags) {
    (void)fd;
    int ret = ensureConnected(c);
    if (ret < 0)
        return ret;
    return PlatformNetSend(c->netCtx, buf, nbyte, flags);
}

static int _SockShutdown(vfs_driver_ctx_t c, int fd, vfs_sdflags_t flags) {
    (void)fd;
    int ret;

    ret = PlatformNetShutdown(c->netCtx, flags);

    return ret;
}
