/* SPDX-License-Identifier: Apache-2.0 */

/* Shared POSIX BSD sockets. TLS (secure sockets) is compiled in only when
 * SECURE_SOCKETS is set (the Linux build with OpenSSL); other targets reject
 * the secure socket types. */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <wanted_malloc.h>
#if SECURE_SOCKETS
#include <network.h>
#endif

struct netCtx {
#if SECURE_SOCKETS
    void *ssl;
    void *sslCtx;
    bool secure;
#endif
    int socket;
    bool isSerial; /* plain device fd: read()/write(), not recv()/send() */
};

void *PlatformNetOpen(int socket_type) {
    int sock;
    int type;

    struct netCtx *netCtx;

    if (socket_type == VFS_SKT_SERIAL) {
        /* The device path isn't known until PlatformNetConnect; defer the
         * real open() there. */
        netCtx = WantedMalloc(sizeof(struct netCtx));
        if (netCtx == NULL) {
            return NULL;
        }
        memset(netCtx, 0, sizeof(struct netCtx));
        netCtx->socket = -1;
        netCtx->isSerial = true;
        return netCtx;
    }

    switch (socket_type) {
    case VFS_SKT_TCP:
        type = SOCK_STREAM;
        break;
    case VFS_SKT_UDP:
        type = SOCK_DGRAM;
        break;
#if SECURE_SOCKETS
    case VFS_SKT_STCP:
        type = SOCK_STREAM;
        break;
    case VFS_SKT_SUDP:
        type = SOCK_DGRAM;
        break;
#else
    case VFS_SKT_STCP:
    case VFS_SKT_SUDP:
        /* Secure sockets require TLS. */
        DEBUG_TRACE("not implemented");
        return NULL;
#endif
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
#if SECURE_SOCKETS
    if (socket_type == VFS_SKT_STCP || socket_type == VFS_SKT_SUDP) {
        netCtx->secure = true;
    }
#endif

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
    const struct hostent *host;
    struct sockaddr_in addr;

    if (NULL == c) {
        return -EINVAL;
    }

    if (c->isSerial) {
        (void)port;
        int fd = open(hostname, O_RDWR | O_NOCTTY);
        if (fd < 0) {
            return -errno;
        }
        c->socket = fd;
        return 0;
    }

    if ((host = gethostbyname(hostname)) == NULL) {
        if (c->socket >= 0) {
            close(c->socket);
            c->socket = -1;
        }
        return -EINVAL;
    }

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, sizeof(addr.sin_addr.s_addr));

    if (connect(c->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        int err = errno;
        if (c->socket >= 0) {
            close(c->socket);
            c->socket = -1;
        }
        return -err;
    }

#if SECURE_SOCKETS
    /* Initialize secure connection */
    if (c->secure) {
        if ((c->sslCtx = TLSInitCtx()) == NULL) {
            if (c->socket >= 0) {
                close(c->socket);
                c->socket = -1;
            }
            return -ENOMEM;
        }

        if ((c->ssl = TLSOpenConnection(c->sslCtx, c->socket)) == NULL) {
            TLSFreeCtx(c->sslCtx);
            if (c->socket >= 0) {
                close(c->socket);
                c->socket = -1;
            }
            return -ECONNREFUSED;
        }
    }
#endif

    return 0;
}

int PlatformNetClose(struct netCtx *c) {
    if (NULL == c) {
        return -EINVAL;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        TLSShutdown(c->ssl);
        TLSFree(c->ssl);
        c->ssl = NULL;
        TLSFreeCtx(c->sslCtx);
        c->sslCtx = NULL;
    }
#endif

    if (c->socket >= 0) {
        close(c->socket);
        c->socket = -1;
    }

    return 0;
}

int PlatformNetRecv(struct netCtx *c, void *buf, size_t nbyte, int flags) {
    int ret;

    if (NULL == c) {
        return -EINVAL;
    }

    if (c->isSerial) {
        (void)flags;
        if ((ret = (int)read(c->socket, buf, nbyte)) < 0) {
            return -errno;
        }
        return ret;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        if ((ret = TLSRead(c->ssl, buf, nbyte)) < 0) {
            return -EIO;
        }
        return ret;
    }
#endif
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

    if (c->isSerial) {
        (void)flags;
        if ((ret = (int)write(c->socket, buf, nbyte)) < 0) {
            return -errno;
        }
        return ret;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        if ((ret = TLSWrite(c->ssl, buf, nbyte)) < 0) {
            return -EIO;
        }
        return ret;
    }
#endif
    if ((ret = send(c->socket, buf, nbyte, flags)) < 0) {
        return -errno;
    }
    return ret;
}

int PlatformNetAccept(struct netCtx *c) {
    int newFd;

    if (NULL == c) {
        return -EINVAL;
    }

    if (c->isSerial) {
        /* A plain serial device fd has no listen/accept model. */
        return -ENOTSUP;
    }

    if ((newFd = accept(c->socket, NULL, NULL)) < 0) {
        return -errno;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        c->ssl = TLSOpenConnection(c->sslCtx, newFd);
        if (c->ssl == NULL) {
            close(newFd);
            return -ECONNREFUSED;
        }
        TLSAccept(c->ssl);
    }
#endif

    return newFd;
}

int PlatformNetShutdown(struct netCtx *c, int how) {
    if (NULL == c) {
        return -EINVAL;
    }

    if (c->isSerial) {
        /* shutdown() isn't defined for a plain device fd; close() is what
         * actually ends the exchange, and the caller does that separately. */
        (void)how;
        return 0;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        TLSShutdown(c->ssl);
    }
#endif

    if (shutdown(c->socket, how) != 0) {
        return -errno;
    }

    return 0;
}
