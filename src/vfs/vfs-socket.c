#include <vfs-drivers.h>
#include <wanted_malloc.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <stdbool.h>

#include <errno.h>

#define MAX_ADDR_LEN 32

static const char id[] = { 'S', 'o', 'c', 'k' };

/* TODO: support many connections */
struct vfs_driver_ctx_t {
    uint8_t         type;
    const char      addr[MAX_ADDR_LEN];
    uint16_t        port;
    vfs_oflags_t    flags;
    bool            connected;
    struct sockaddr_in serv_addr;
};

static int _Destroy     (struct vfs_driver_t *d);
static int _Open        (vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt      (vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close       (vfs_driver_ctx_t d, int fd);
static int _Read        (vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write       (vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Stat        (vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _SockAccept  (vfs_driver_ctx_t d, int fd, vfs_oflags_t flags, int *newFd);
static int _SockRecv    (vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte, vfs_riflags_t iflags, vfs_roflags_t *oflags);
static int _SockSend    (vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte, vfs_sdflags_t flags);
static int _SockShutdown(vfs_driver_ctx_t d, int fd, vfs_sdflags_t flags);

static vfs_filetype_t convertSocketType(uint8_t type) {
    switch(type) {
    case VFS_SKT_BUS:
        return VFS_FILETYPE_SOCKET_STREAM;
    case VFS_SKT_TCP:
        return VFS_FILETYPE_SOCKET_STREAM;
    case VFS_SKT_UDP:
        return VFS_FILETYPE_SOCKET_DGRAM;
    default:
        return VFS_FILETYPE_UNKNOWN;
    }
}

vfs_driver_t *VfsSocketInit(uint8_t type, char *addr, uint16_t port)
{
    int ret;
    vfs_driver_t *driver;

    if ( NULL == addr) {
        return NULL;
    }

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        WantedFree(driver);
        return NULL;
    }

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = convertSocketType(type);
    driver->ctx->type       = type;
    strncpy((char *)driver->ctx->addr, addr, MAX_ADDR_LEN);
    driver->ctx->port       = port;
    driver->Destroy         = _Destroy;
    driver->Open            = _Open;
    driver->OpenAt          = _OpenAt;
    driver->Close           = _Close;
    driver->Read            = _Read;
    driver->Write           = _Write;
    driver->Stat            = _Stat;
    driver->SockAccept      = _SockAccept;
    driver->SockRecv        = _SockRecv;
    driver->SockSend        = _SockSend;
    driver->SockShutdown    = _SockShutdown;

    return driver;
}

static int _Destroy (struct vfs_driver_t *d)
{
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int ConnectSocket(vfs_driver_ctx_t d, int sock)
{
    if (d->connected) {
        return 0;
    }

    if (connect(sock, (struct sockaddr *)&d->serv_addr, sizeof(d->serv_addr)) < 0)
    {
        return -errno;
    }
    d->connected = true;
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    int sock = 0, ret, socket_type;

    if (d->connected) {
        return -EISCONN;
    }

    switch (d->type)
    {
    case VFS_SKT_TCP:
        socket_type = SOCK_STREAM;
        break;
    case VFS_SKT_UDP:
        socket_type = SOCK_DGRAM;
        break;
    default:
        return -ESOCKTNOSUPPORT;
        break;
    }

    if ((sock = socket(AF_INET, socket_type, 0)) < 0) {
        return -errno;
    }

    d->serv_addr.sin_family = AF_INET;
    d->serv_addr.sin_port = htons(d->port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if((ret = inet_pton(AF_INET, d->addr, &d->serv_addr.sin_addr)) < 0)
    {
        return -errno;
    } else if (ret == 0) {
        return -EINVAL;
    }

    d->connected = false;
    d->flags = flags;

    return sock;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    return _Open(d, path, flags);
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    d->connected = false;
    return close(fd);
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    int ret;

    ret = ConnectSocket(d, fd);
    if (ret < 0) return ret;

    ret = read(fd, buf, nbyte);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    int ret;

    ret = ConnectSocket(d, fd);
    if (ret < 0) return ret;

    ret = write(fd, buf, nbyte);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    int ret;
    if (NULL == stat) return -EINVAL;

    stat->dev = *(uint32_t *)id;
    stat->ino = d->port;
    stat->filetype = convertSocketType(d->type);
    stat->size = d->connected;
    stat->atim = 0;
    stat->mtim = 0;
    stat->ctim = 0;
    stat->oflags = d->flags;

    return 0;
}

static int _SockAccept  (vfs_driver_ctx_t d, int fd, vfs_oflags_t flags, int *newFd)
{
    if (newFd == NULL) {
        return -EINVAL;
    }

    *newFd = accept(fd, NULL, NULL);
    if (*newFd < 0) {
        return -errno;
    }

    return 0;
}

static int _SockRecv(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte, vfs_riflags_t iflags, vfs_roflags_t *oflags)
{
    int ret;

    ret = ConnectSocket(d, fd);
    if (ret < 0) return ret;

    ret = recv(fd, buf, nbyte, iflags);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _SockSend(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte, vfs_sdflags_t flags)
{
    int ret;

    ret = ConnectSocket(d, fd);
    if (ret < 0) return ret;

    ret = send(fd , buf , nbyte , flags);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _SockShutdown(vfs_driver_ctx_t d, int fd, vfs_sdflags_t flags)
{
    int ret = shutdown(fd, flags);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}
