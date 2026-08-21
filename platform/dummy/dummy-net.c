/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>

#include <platform.h>

#include "dummy-fs.h"

/* In-memory network mock — a deterministic stand-in for the host stack, which
 * is what makes the VFS socket driver unit-testable. Buffers are per socket,
 * so a listener and its connections carry streams a test can keep apart. */

#define DUMMY_NET_MAX_SOCKS 8
#define DUMMY_NET_BUF 256

struct netCtx {
    int used;
    int type;
    int listening;
    uint8_t rx[DUMMY_NET_BUF];
    size_t rx_len;
    size_t rx_off;
    uint8_t tx[DUMMY_NET_BUF];
    size_t tx_len;
};

static struct netCtx g_socks[DUMMY_NET_MAX_SOCKS];
static struct netCtx *g_last;

static int g_open_fail;
static int g_connect_result;
static int g_listen_result;
static int g_accept_result;

/* ── Test control ───────────────────────────────────────────────────────── */

void DummyNetReset(void) {
    memset(g_socks, 0, sizeof(g_socks));
    g_last = NULL;
    g_open_fail = 0;
    g_connect_result = 0;
    g_listen_result = 0;
    g_accept_result = 0;
}

void DummyNetSetOpenFail(int fail) { g_open_fail = fail; }
void DummyNetSetConnectResult(int result) { g_connect_result = result; }
void DummyNetSetListenResult(int result) { g_listen_result = result; }
void DummyNetSetAcceptResult(int result) { g_accept_result = result; }

void DummyNetSeedRecvOn(void *ctx, const uint8_t *buf, size_t len) {
    struct netCtx *s = ctx;
    if (!s || !buf)
        return;
    if (len > sizeof(s->rx))
        len = sizeof(s->rx);
    memcpy(s->rx, buf, len);
    s->rx_len = len;
    s->rx_off = 0;
}

size_t DummyNetGetSentOn(void *ctx, uint8_t *buf, size_t len) {
    struct netCtx *s = ctx;
    if (!s)
        return 0;
    size_t n = s->tx_len < len ? s->tx_len : len;
    if (buf && n > 0)
        memcpy(buf, s->tx, n);
    return s->tx_len;
}

void DummyNetSeedRecv(const uint8_t *buf, size_t len) {
    DummyNetSeedRecvOn(g_last, buf, len);
}

size_t DummyNetGetSent(uint8_t *buf, size_t len) {
    return DummyNetGetSentOn(g_last, buf, len);
}

void *DummyNetLastSock(void) { return g_last; }

/* ── PlatformNet* mock ──────────────────────────────────────────────────── */

static struct netCtx *allocSock(int socket_type) {
    for (int i = 0; i < DUMMY_NET_MAX_SOCKS; i++) {
        if (!g_socks[i].used) {
            memset(&g_socks[i], 0, sizeof(g_socks[i]));
            g_socks[i].used = 1;
            g_socks[i].type = socket_type;
            g_last = &g_socks[i];
            return &g_socks[i];
        }
    }
    return NULL;
}

struct netCtx *PlatformNetOpen(int socket_type) {
    if (g_open_fail)
        return NULL;
    return allocSock(socket_type);
}

int PlatformNetConnect(struct netCtx *ctx, const char *hostname,
                       uint16_t port) {
    (void)ctx;
    (void)hostname;
    (void)port;
    return g_connect_result;
}

int PlatformNetListen(struct netCtx *ctx, const char *bindAddr, uint16_t port,
                      int backlog) {
    (void)bindAddr;
    (void)port;
    (void)backlog;
    if (!ctx)
        return -EINVAL;
    if (g_listen_result == 0)
        ctx->listening = 1;
    return g_listen_result;
}

/* `ctx` is the listening socket the platform API hands every backend; this mock
 * only reads it. */
/* cppcheck-suppress constParameterPointer */
/* The test platform has no real queue: a raised wake answers -EINTR, and
 * anything else lets the call below decide. */
int PlatformNetWaitReadable(struct netCtx *ctx, int wakeFd,
                            int64_t timeout_ns) {
    (void)timeout_ns;
    if (!ctx)
        return -EINVAL;
    if (wakeFd < 0)
        return 0;

    fd_set r;
    struct timeval tv = {0, 0};
    FD_ZERO(&r);
    FD_SET(wakeFd, &r);
    if (select(wakeFd + 1, &r, NULL, NULL, &tv) > 0 && FD_ISSET(wakeFd, &r))
        return -EINTR;
    return 0;
}

/* `ctx` is only read here; the signature matches the platform API's fixed
 * prototype. */
/* cppcheck-suppress constParameterPointer */
int PlatformNetAccept(struct netCtx *ctx, struct netCtx **out) {
    if (!ctx || !out)
        return -EINVAL;
    if (g_accept_result < 0)
        return g_accept_result;

    struct netCtx *conn = allocSock(ctx->type);
    if (!conn)
        return -ENFILE;

    *out = conn;
    return 0;
}

int PlatformNetClose(struct netCtx *ctx) {
    (void)ctx;
    return 0;
}

int PlatformNetRecv(struct netCtx *ctx, void *buf, size_t nbyte, int flags) {
    (void)flags;
    if (!ctx || !buf)
        return 0;
    size_t avail = ctx->rx_len - ctx->rx_off;
    size_t n = nbyte < avail ? nbyte : avail;
    if (n > 0) {
        memcpy(buf, ctx->rx + ctx->rx_off, n);
        ctx->rx_off += n;
    }
    return (int)n;
}

int PlatformNetSend(struct netCtx *ctx, const void *buf, size_t nbyte,
                    int flags) {
    (void)flags;
    if (!ctx || !buf)
        return 0;
    size_t space = sizeof(ctx->tx) - ctx->tx_len;
    size_t n = nbyte < space ? nbyte : space;
    if (n > 0) {
        memcpy(ctx->tx + ctx->tx_len, buf, n);
        ctx->tx_len += n;
    }
    return (int)nbyte;
}

int PlatformNetShutdown(struct netCtx *ctx, int how) {
    (void)ctx;
    (void)how;
    return 0;
}

int PlatformNetFree(struct netCtx *ctx) {
    if (ctx) {
        if (g_last == ctx)
            g_last = NULL;
        ctx->used = 0;
    }
    return 0;
}
