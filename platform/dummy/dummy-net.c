/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <platform.h>

#include "dummy-fs.h"

/* ── In-memory network mock ──────────────────────────────────────────────────
 * Deterministic stand-in for the host network stack. The VFS socket driver
 * delegates every operation to PlatformNet*, so this mock is what makes that
 * driver unit-testable: PlatformNetOpen hands out pool slots, Recv drains a
 * test-seeded inbound buffer, Send captures bytes for inspection, and the
 * connect/accept results are test-controllable. "Single connection" matches
 * the socket driver's own TODO, so the rx/tx buffers are global rather than
 * per-socket.
 * ───────────────────────────────────────────────────────────────────────── */

#define DUMMY_NET_MAX_SOCKS 4
#define DUMMY_NET_BUF       256

typedef struct {
    int used;
    int type;
} dummy_net_sock_t;

static dummy_net_sock_t g_socks[DUMMY_NET_MAX_SOCKS];

static int g_open_fail;
static int g_connect_result;
static int g_accept_result;

static uint8_t g_rx[DUMMY_NET_BUF];
static size_t  g_rx_len;
static size_t  g_rx_off;

static uint8_t g_tx[DUMMY_NET_BUF];
static size_t  g_tx_len;

/* ── Test control ───────────────────────────────────────────────────────── */

void DummyNetReset(void) {
    memset(g_socks, 0, sizeof(g_socks));
    g_open_fail       = 0;
    g_connect_result  = 0;
    g_accept_result   = 0;
    memset(g_rx, 0, sizeof(g_rx));
    g_rx_len = 0;
    g_rx_off = 0;
    memset(g_tx, 0, sizeof(g_tx));
    g_tx_len = 0;
}

void DummyNetSetOpenFail(int fail)       { g_open_fail = fail; }
void DummyNetSetConnectResult(int result){ g_connect_result = result; }
void DummyNetSetAcceptResult(int result) { g_accept_result = result; }

void DummyNetSeedRecv(const uint8_t *buf, size_t len) {
    if (!buf)
        return;
    if (len > sizeof(g_rx))
        len = sizeof(g_rx);
    memcpy(g_rx, buf, len);
    g_rx_len = len;
    g_rx_off = 0;
}

size_t DummyNetGetSent(uint8_t *buf, size_t len) {
    size_t n = g_tx_len < len ? g_tx_len : len;
    if (buf && n > 0)
        memcpy(buf, g_tx, n);
    return g_tx_len;
}

/* ── PlatformNet* mock ──────────────────────────────────────────────────── */

void *PlatformNetOpen(int socket_type) {
    if (g_open_fail)
        return NULL;
    for (int i = 0; i < DUMMY_NET_MAX_SOCKS; i++) {
        if (!g_socks[i].used) {
            g_socks[i].used = 1;
            g_socks[i].type = socket_type;
            return &g_socks[i];
        }
    }
    return NULL;
}

int PlatformNetConnect(void *ctx, const char *hostname, uint16_t port) {
    (void)ctx;
    (void)hostname;
    (void)port;
    return g_connect_result;
}

int PlatformNetClose(void *ctx) {
    (void)ctx;
    return 0;
}

int PlatformNetRecv(void *ctx, void *buf, size_t nbyte, int flags) {
    (void)ctx;
    (void)flags;
    if (!buf)
        return 0;
    size_t avail = g_rx_len - g_rx_off;
    size_t n = nbyte < avail ? nbyte : avail;
    if (n > 0) {
        memcpy(buf, g_rx + g_rx_off, n);
        g_rx_off += n;
    }
    return (int)n;
}

int PlatformNetSend(void *ctx, const void *buf, size_t nbyte, int flags) {
    (void)ctx;
    (void)flags;
    if (!buf)
        return 0;
    size_t space = sizeof(g_tx) - g_tx_len;
    size_t n = nbyte < space ? nbyte : space;
    if (n > 0) {
        memcpy(g_tx + g_tx_len, buf, n);
        g_tx_len += n;
    }
    return (int)nbyte;
}

int PlatformNetAccept(void *ctx) {
    (void)ctx;
    return g_accept_result;
}

int PlatformNetShutdown(void *ctx, int how) {
    (void)ctx;
    (void)how;
    return 0;
}

int PlatformNetFree(void *ctx) {
    if (ctx) {
        dummy_net_sock_t *s = (dummy_net_sock_t *)ctx;
        s->used = 0;
    }
    return 0;
}
