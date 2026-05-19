#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>

#include <platform.h>

// TODO: make configurable, max address length should be defined
#define MAX_ADDR_LEN 32

static const char id[] = {'S', 'o', 'c', 'k'};

/* TODO: support many connections */
struct vfs_driver_ctx_t {
    uint8_t type;
    const char addr[MAX_ADDR_LEN];
    uint16_t port;
    vfs_oflags_t flags;
    bool connected;
    void *netCtx;
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _SockAccept(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags,
                       int *newFd);
static int _SockRecv(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte,
                     vfs_riflags_t iflags, vfs_roflags_t *oflags);
static int _SockSend(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte,
                     vfs_sdflags_t flags);
static int _SockShutdown(vfs_driver_ctx_t d, int fd, vfs_sdflags_t flags);

static vfs_filetype_t convertSocketType(uint8_t type) {
    switch (type) {
    case VFS_SKT_BUS:
        return VFS_FILETYPE_SOCKET_STREAM;
    case VFS_SKT_TCP:
    case VFS_SKT_STCP:
        return VFS_FILETYPE_SOCKET_STREAM;
    case VFS_SKT_UDP:
    case VFS_SKT_SUDP:
        return VFS_FILETYPE_SOCKET_DGRAM;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

vfs_driver_t *VfsSocketInit(const wapp_t *wapp, const char *options) {
    int ret;
    char t;
    uint16_t port;
    vfs_driver_t *driver;

    if (NULL == options) {
        DEBUG_TRACE("bad options");
        return NULL;
    }

    char addr[strnlen(options, MAX_ADDR_LEN)];

    ret = sscanf(options, "%c %s %hd", &t, addr, &port);
    if (ret < 3) {
        DEBUG_TRACE("error during parsing options");
        return NULL;
    }

    uint8_t type;
    switch (t) {
    case 'T':
        type = VFS_SKT_STCP;
        break;
    case 'U':
        type = VFS_SKT_SUDP;
        break;
    case 't':
        type = VFS_SKT_TCP;
        break;
    case 'u':
        type = VFS_SKT_UDP;
        break;
    case 'b':
        type = VFS_SKT_BUS;
        break;
    default:
        DEBUG_TRACE("error during parsing socket type");
        return NULL;
    }

    if (!SECURE_SOCKETS && (type == VFS_SKT_STCP || type == VFS_SKT_SUDP)) {
        DEBUG_TRACE("no support for secure sockets");
        return NULL;
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
    bzero(driver->ctx, sizeof(struct vfs_driver_ctx_t));

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
    return _Open(c, path, flags);
}

static int _Close(vfs_driver_ctx_t c, int fd) {
    c->connected = false;
    return PlatformNetClose(c->netCtx);
}

static int _Read(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte) {
    int ret;

    if (!c->connected) {
        if ((ret = PlatformNetConnect(c->netCtx, c->addr, c->port)) < 0) {
            return ret;
        }
        c->connected = true;
    }

    ret = PlatformNetRecv(c->netCtx, buf, nbyte, 0);

    return ret;
}

static int _Write(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte) {
    int ret;

    if (!c->connected) {
        if ((ret = PlatformNetConnect(c->netCtx, c->addr, c->port)) < 0) {
            return ret;
        }
        c->connected = true;
    }

    ret = PlatformNetSend(c->netCtx, buf, nbyte, 0);

    return ret;
}

static int _Stat(vfs_driver_ctx_t c, int fd, vfs_stat_t *stat) {
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
    int ret;
    if (newFd == NULL) {
        return -EINVAL;
    }

    // TODO: missing some stuff here

    ret = PlatformNetAccept(c->netCtx);

    if (ret >= 0)
        *newFd = ret;

    return ret;
}

static int _SockRecv(vfs_driver_ctx_t c, int fd, void *buf, size_t nbyte,
                     vfs_riflags_t iflags, vfs_roflags_t *oflags) {
    int ret;

    if (!c->connected) {
        if ((ret = PlatformNetConnect(c->netCtx, c->addr, c->port)) < 0) {
            return ret;
        }
        c->connected = true;
    }

    ret = PlatformNetRecv(c->netCtx, buf, nbyte, iflags);

    return ret;
}

static int _SockSend(vfs_driver_ctx_t c, int fd, const void *buf, size_t nbyte,
                     vfs_sdflags_t flags) {
    int ret;

    if (!c->connected) {
        if ((ret = PlatformNetConnect(c->netCtx, c->addr, c->port)) < 0) {
            return ret;
        }
        c->connected = true;
    }

    ret = PlatformNetSend(c->netCtx, buf, nbyte, flags);

    return ret;
}

static int _SockShutdown(vfs_driver_ctx_t c, int fd, vfs_sdflags_t flags) {
    int ret;

    ret = PlatformNetShutdown(c->netCtx, flags);

    return ret;
}
