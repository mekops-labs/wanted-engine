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
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
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
    bool dgram;
    /* Datagram peer, learned from the last recvfrom() on a bound socket: the
     * VFS carries no address alongside a payload, so a bound datagram socket
     * answers whoever it last heard from. */
    bool hasPeer;
    struct sockaddr_in peer;
};

struct netCtx *PlatformNetOpen(int socket_type) {
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
    netCtx->dgram = (type == SOCK_DGRAM);
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

/* Put a termios into raw mode: no input translation or flow control, no output
 * post-processing, no canonical buffering, echo or signal generation, 8-bit
 * characters with no parity. Every flag used here is POSIX. */
static void makeRaw(struct termios *t) {
    t->c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                              IGNCR | ICRNL | IXON);
    t->c_oflag &= ~(tcflag_t)OPOST;
    t->c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    t->c_cflag |= (tcflag_t)CS8;
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

        /* A serial:// socket is opened fresh for every request/response round,
         * so put the line into raw mode and flush the RX buffer: a late or
         * partially-drained previous response would desync the next fetch. */
        struct termios tio;
        if (tcgetattr(fd, &tio) == 0) {
            makeRaw(&tio);
            tio.c_cc[VMIN] = 1;
            tio.c_cc[VTIME] = 0;
            (void)tcsetattr(fd, TCSANOW, &tio);
        }
        (void)tcflush(fd, TCIFLUSH);

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

int PlatformNetListen(struct netCtx *c, const char *bindAddr, uint16_t port,
                      int backlog) {
    const struct hostent *host;
    struct sockaddr_in addr;
    int on = 1;

    if (NULL == c || NULL == bindAddr) {
        return -EINVAL;
    }

    if (c->isSerial) {
        return -ENOTSUP;
    }

#if SECURE_SOCKETS
    if (c->secure) {
        /* A TLS server needs a certificate and a private key, and nothing
         * supplies them. */
        return -ENOTSUP;
    }
#endif

    if ((host = gethostbyname(bindAddr)) == NULL) {
        return -EINVAL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, host->h_addr, sizeof(addr.sin_addr.s_addr));

    /* Rebind straight after a restart rather than waiting out TIME_WAIT. */
    (void)setsockopt(c->socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    if (bind(c->socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        return -errno;
    }

    if (c->dgram) {
        return 0;
    }

    if (listen(c->socket, backlog) != 0) {
        return -errno;
    }

    return 0;
}

int PlatformNetWaitReadable(struct netCtx *c, int wakeFd) {
    fd_set r;
    int high;

    if (NULL == c) {
        return -EINVAL;
    }
    if (wakeFd < 0) {
        /* No wake descriptor: the call below blocks and a signal ends it. */
        return 0;
    }

    high = c->socket > wakeFd ? c->socket : wakeFd;
    for (;;) {
        FD_ZERO(&r);
        FD_SET(c->socket, &r);
        FD_SET(wakeFd, &r);

        if (select(high + 1, &r, NULL, NULL, NULL) < 0) {
            if (errno == EINTR) {
                return -EINTR;
            }
            return -errno;
        }
        /* The wake first: a stop outranks a connection that also arrived. */
        if (FD_ISSET(wakeFd, &r)) {
            return -EINTR;
        }
        if (FD_ISSET(c->socket, &r)) {
            return 0;
        }
    }
}

int PlatformNetAccept(struct netCtx *c, struct netCtx **out) {
    struct netCtx *conn;
    int newFd;

    if (NULL == c || NULL == out) {
        return -EINVAL;
    }

    if (c->isSerial || c->dgram) {
        /* Neither a device fd nor a datagram socket has an accept queue. */
        return -ENOTSUP;
    }

    if ((newFd = accept(c->socket, NULL, NULL)) < 0) {
        return -errno;
    }

    conn = WantedMalloc(sizeof(struct netCtx));
    if (conn == NULL) {
        close(newFd);
        return -ENOMEM;
    }
    memset(conn, 0, sizeof(struct netCtx));
    conn->socket = newFd;

    *out = conn;
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
    if (c->dgram) {
        struct sockaddr_in from;
        socklen_t fromLen = sizeof(from);
        ret = (int)recvfrom(c->socket, buf, nbyte, flags,
                            (struct sockaddr *)&from, &fromLen);
        if (ret < 0) {
            return -errno;
        }
        if (fromLen == sizeof(from)) {
            c->peer = from;
            c->hasPeer = true;
        }
        return ret;
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
    if (c->dgram && c->hasPeer) {
        ret = (int)sendto(c->socket, buf, nbyte, flags,
                          (const struct sockaddr *)&c->peer, sizeof(c->peer));
        if (ret < 0) {
            return -errno;
        }
        return ret;
    }

    if ((ret = send(c->socket, buf, nbyte, flags)) < 0) {
        return -errno;
    }
    return ret;
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
