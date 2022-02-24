#include <vfs-drivers.h>
#include <wanted_malloc.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>

#include <errno.h>

#define MAX_ADDR_LEN 32

static const char id[] = { 'S', 'o', 'c', 'k' };

struct vfs_driver_ctx_t {
    uint8_t         type;
    const char      addr[MAX_ADDR_LEN];
    uint16_t        port;
    vfs_oflags_t    flags;
};

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

int VfsSocketInit(vfs_driver_t *driver, uint8_t type, char *addr, uint16_t port)
{
    int ret;

    if (NULL == driver || NULL == addr) {
        return -EINVAL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) return -ENOMEM;

    driver->bytesId         = *(uint32_t*)(id);
    driver->filetype        = convertSocketType(type);
    driver->ctx->type       = type;
    strncpy((char *)driver->ctx->addr, addr, MAX_ADDR_LEN);
    driver->ctx->port       = port;
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
    return 0;
}

void VfsSocketDestroy(vfs_driver_t *driver)
{
    WantedFree(driver->ctx);
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    int sock = 0, ret, socket_type;
    struct sockaddr_in serv_addr;

    switch (d->type)
    {
    case VFS_SKT_TCP:
        socket_type = SOCK_STREAM;
        break;
    case VFS_SKT_UDP:
        socket_type = SOCK_DGRAM;
    default:
        return -ESOCKTNOSUPPORT;
        break;
    }

    if ((sock = socket(AF_INET, socket_type, 0)) < 0) {
        return -errno;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(d->port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if((ret = inet_pton(AF_INET, d->addr, &serv_addr.sin_addr)) < 0)
    {
        return -errno;
    } else if (ret == 0) {
        return -EINVAL;
    }

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        return -errno;
    }

    d->flags = flags;

    return sock;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags)
{
    return _Open(d, path, flags);
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    return close(fd);
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    int ret = read(fd, buf, nbyte);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    int ret = write(fd, buf, nbyte);
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
    stat->size = 0;
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
    int ret = recv(fd, buf, nbyte, iflags);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}

static int _SockSend(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte, vfs_sdflags_t flags)
{
    int ret = send(fd , buf , nbyte , flags);
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
