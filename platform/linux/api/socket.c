#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <resolv.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <network.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>

struct netCtx {
    void *ssl;
    void *sslCtx;
    bool secure;
    int socket;
};

void *PlatformNetOpen(int socket_type) {
    int sock;
    int type;
    int ret;

    struct netCtx *netCtx;

    switch (socket_type) {
    case VFS_SKT_TCP:
    case VFS_SKT_STCP:
        type = SOCK_STREAM;
        break;
    case VFS_SKT_UDP:
    case VFS_SKT_SUDP:
        type = SOCK_DGRAM;
        break;
    default:
        return NULL;
        break;
    }

    if ((sock = socket(AF_INET, type, 0)) < 0) {
        return NULL;
    }

    netCtx = WantedMalloc(sizeof(struct netCtx));
    if (netCtx == NULL) {
        close(sock);
        return NULL;
    }
    bzero(netCtx, sizeof(struct netCtx));

    netCtx->socket = sock;
    if (socket_type == VFS_SKT_STCP || socket_type == VFS_SKT_SUDP) {
        netCtx->secure = true;
    }

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

    bzero(&addr, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = *(long *)(host->h_addr);

    if (connect(c->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -errno;
    }

    /* Initialize secure connection */
    if (c->secure) {
        if ((c->sslCtx = TLSInitCtx()) == NULL) {
            return -ENOMEM;
        }

        if ((c->ssl = TLSOpenConnection(c->sslCtx, c->socket)) == NULL) {
            TLSFreeCtx(c->sslCtx);
            return -ECONNREFUSED;
        }
    }

    return 0;
}

int PlatformNetClose(struct netCtx *c) {
    if (NULL == c) {
        return -EINVAL;
    }

    if (c->secure) {
        TLSShutdown(c->ssl);
        TLSFree(c->ssl);
        c->ssl = NULL;
        TLSFreeCtx(c->sslCtx);
        c->sslCtx = NULL;
    }

    close(c->socket);

    return 0;
}

int PlatformNetRecv(struct netCtx *c, void *buf, size_t nbyte, int flags) {
    int ret;

    if (NULL == c) {
        return -EINVAL;
    }

    if (c->secure) {
        if ((ret = TLSRead(c->ssl, buf, nbyte)) < 0) {
            return -EIO;
        }
    } else {
        if ((ret = recv(c->socket, buf, nbyte, flags)) < 0) {
            return -errno;
        }
    }
    return ret;
}

int PlatformNetSend(struct netCtx *c, const void *buf, size_t nbyte,
                    int flags) {
    int ret;

    if (NULL == c) {
        return -EINVAL;
    }

    if (c->secure) {
        if ((ret = TLSWrite(c->ssl, buf, nbyte)) < 0) {
            return -EIO;
        }
    } else {
        if ((ret = send(c->socket, buf, nbyte, flags)) < 0) {
            return -errno;
        }
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

    if (c->secure) {
        c->ssl = TLSOpenConnection(c->sslCtx, newFd);
        if (c->ssl == NULL) {
            return -ECONNREFUSED;
        }
        TLSAccept(c->ssl);
    }

    return newFd;
}

int PlatformNetShutdown(struct netCtx *c, int how) {
    if (NULL == c) {
        return -EINVAL;
    }

    if (c->secure) {
        TLSShutdown(c->ssl);
    }

    if (shutdown(c->socket, how) != 0) {
        return -errno;
    }

    return 0;
}