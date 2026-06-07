/* NuttX platform sockets
 *
 * The BSD socket API is the same as on Linux. */

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>

struct netCtx {
    int socket;
};

void *PlatformNetOpen(int socket_type) {
    int sock;
    int type;

    struct netCtx *netCtx;

    switch (socket_type) {
    case VFS_SKT_TCP:
        type = SOCK_STREAM;
        break;
    case VFS_SKT_UDP:
        type = SOCK_DGRAM;
        break;
    case VFS_SKT_STCP:
    case VFS_SKT_SUDP:
        /* Secure sockets require TLS. */
        DEBUG_TRACE("not implemented");
        return NULL;
    default:
        return NULL;
    }

    if ((sock = socket(AF_INET, type, 0)) < 0) {
        return NULL;
    }

    netCtx = WantedMalloc(sizeof(struct netCtx));
    if (netCtx == NULL) {
        close(sock);
        return NULL;
    }
    memset(netCtx, 0, sizeof(struct netCtx));

    netCtx->socket = sock;

    return netCtx;
}

int PlatformNetFree(struct netCtx *c) {
    if (c == NULL) {
        return -EINVAL;
    }

    WantedFree(c);

    return 0;
}

int PlatformNetConnect(struct netCtx *c, const char *hostname, uint16_t port) {
    struct hostent *host;
    struct sockaddr_in addr;

    if (NULL == c) {
        return -EINVAL;
    }

    if ((host = gethostbyname(hostname)) == NULL) {
        return -EINVAL;
    }

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, sizeof(addr.sin_addr.s_addr));

    if (connect(c->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -errno;
    }

    return 0;
}

int PlatformNetClose(struct netCtx *c) {
    if (NULL == c) {
        return -EINVAL;
    }

    close(c->socket);

    return 0;
}

int PlatformNetRecv(struct netCtx *c, void *buf, size_t nbyte, int flags) {
    int ret;

    if (NULL == c) {
        return -EINVAL;
    }

    if ((ret = recv(c->socket, buf, nbyte, flags)) < 0) {
        return -errno;
    }

    return ret;
}

int PlatformNetSend(struct netCtx *c, const void *buf, size_t nbyte,
                    int flags) {
    int ret;

    if (NULL == c) {
        return -EINVAL;
    }

    if ((ret = send(c->socket, buf, nbyte, flags)) < 0) {
        return -errno;
    }

    return ret;
}

/* TODO: totally untested, broken */
int PlatformNetAccept(struct netCtx *c) {
    int newFd;

    if (NULL == c) {
        return -EINVAL;
    }

    if ((newFd = accept(c->socket, NULL, NULL)) < 0) {
        return -errno;
    }

    return newFd;
}

int PlatformNetShutdown(struct netCtx *c, int how) {
    if (NULL == c) {
        return -EINVAL;
    }

    if (shutdown(c->socket, how) != 0) {
        return -errno;
    }

    return 0;
}
