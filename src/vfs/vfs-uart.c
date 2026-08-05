/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* UART device driver: a per-port subtree under /dev/uart.
 *
 *   /dev/uart/
 *     <port>/
 *       data    raw byte stream
 *       baud    decimal rate, e.g. "921600"
 *       format  <databits><parity><stopbits>, e.g. "8N1"
 *
 * The grant names one port and its initial line configuration:
 *
 *   { "name": "uart", "options": "port=1,tx=1,rx=2,baud=57600,format=8E1" }
 *
 * port= is both the backing's port identity and the wapp-visible directory
 * name. Every remaining key is platform addressing and goes to the backing
 * untouched, so a wapp built against /dev/uart/1/data runs unchanged on every
 * target.
 *
 * baud and format are writable at runtime, because one link carries two
 * settings: a bootloader sync and the framed channel that follows it disagree
 * on both rate and parity. A write drains the transmit buffer, then discards
 * the receive buffer — bytes clocked in under the old settings cannot be
 * decoded under the new ones.
 *
 * One wapp holds a port, exclusively. Multiplexing several logical users onto
 * one physical link is a broker wapp's job, not the engine's: the engine would
 * need this peripheral's framing and correlation-ID scheme to do it, and that
 * knowledge is useless to every other UART device.
 *
 * A blocking read on `data` returns on a byte or on -EINTR, with no wall-clock
 * cap. An idle line is a UART's normal state and says nothing about a fault,
 * so there is no elapsed time from which a timeout could be inferred. A wapp
 * that wants a deadline opens with O_NONBLOCK and runs its own clock. */

static const char id[] = {'U', 'a', 'r', 't'};

#define UART_MAX_FDS 8
#define UART_PORT_NAME_MAX WAPP_MAX_NAME_LEN
#define UART_PLAT_OPTS_MAX 96
#define UART_LINE_MAX 16

/* Blocking-I/O poll cadence. The wait is uncapped, so a stop must interrupt
 * the sleep for the wapp to unwind. */
#define UART_POLL_INTERVAL_NS 1000000ULL /* 1 ms */

#define UART_NODE_ROOT 0
#define UART_NODE_PORT 1
#define UART_NODE_DATA 2
#define UART_NODE_BAUD 3
#define UART_NODE_FORMAT 4

struct uart_fd_t {
    bool used;
    uint8_t node;
    bool nonblock;
    bool read_done; /* EOF latch, attribute nodes only */
};

struct vfs_driver_ctx_t {
    char port[UART_PORT_NAME_MAX + 1];
    char plat_opts[UART_PLAT_OPTS_MAX];
    platform_uart_t *uart;
    uint32_t baud;
    uint8_t databits;
    uint8_t parity;
    uint8_t stopbits;
    struct uart_fd_t fds[UART_MAX_FDS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

/* ── Value grammars ──────────────────────────────────────────────────────── */

/* Decimal, no sign, no leading blank. Returns 0 or -EINVAL. */
static int parseDecimal(const char *s, size_t len, uint32_t *out) {
    if (len == 0 || len > 9)
        return -EINVAL;
    uint32_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -EINVAL;
        n = n * 10 + (uint32_t)(s[i] - '0');
    }
    *out = n;
    return 0;
}

/* "<databits><parity><stopbits>", e.g. "8N1". The grammar admits combinations
 * no backing supports; the backing rejects those at apply time. */
static int parseFormat(const char *s, size_t len, uint8_t *databits,
                       uint8_t *parity, uint8_t *stopbits) {
    if (len != 3)
        return -EINVAL;
    if (s[0] < '5' || s[0] > '8')
        return -EINVAL;
    uint8_t p;
    switch (s[1]) {
    case 'N':
    case 'n':
        p = 'N';
        break;
    case 'E':
    case 'e':
        p = 'E';
        break;
    case 'O':
    case 'o':
        p = 'O';
        break;
    default:
        return -EINVAL;
    }
    if (s[2] != '1' && s[2] != '2')
        return -EINVAL;

    *databits = (uint8_t)(s[0] - '0');
    *parity = p;
    *stopbits = (uint8_t)(s[2] - '0');
    return 0;
}

/* A port name is [A-Za-z0-9_-], 1..UART_PORT_NAME_MAX characters — the same
 * grammar every named resource in a launch config uses. */
static bool validPort(const char *s, size_t len) {
    if (len == 0 || len > UART_PORT_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse the grant. port=, baud= and format= are consumed here; every other
 * key is appended to plat_opts for the backing to accept or reject. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    if (options == NULL || options[0] == '\0') {
        DEBUG_TRACE("uart grant is empty");
        return -EINVAL;
    }

    ctx->baud = 115200;
    ctx->databits = 8;
    ctx->parity = 'N';
    ctx->stopbits = 1;

    bool have_port = false;
    size_t optsLen = 0;
    const char *p = options;

    while (*p != '\0') {
        const char *kv = p;
        while (*p != '\0' && *p != ',')
            p++;
        size_t kvLen = (size_t)(p - kv);
        if (*p == ',')
            p++;
        if (kvLen == 0)
            return -EINVAL;

        const char *eq = kv;
        while (eq < kv + kvLen && *eq != '=')
            eq++;
        if (eq == kv + kvLen)
            return -EINVAL; /* every clause is key=value */

        size_t keyLen = (size_t)(eq - kv);
        const char *val = eq + 1;
        size_t valLen = kvLen - keyLen - 1;

        if (keyLen == 4 && memcmp(kv, "port", 4) == 0) {
            if (have_port) {
                DEBUG_TRACE("uart grant names more than one port");
                return -EINVAL;
            }
            if (!validPort(val, valLen))
                return -EINVAL;
            memcpy(ctx->port, val, valLen);
            ctx->port[valLen] = '\0';
            have_port = true;
        } else if (keyLen == 4 && memcmp(kv, "baud", 4) == 0) {
            if (parseDecimal(val, valLen, &ctx->baud) < 0 || ctx->baud == 0)
                return -EINVAL;
        } else if (keyLen == 6 && memcmp(kv, "format", 6) == 0) {
            if (parseFormat(val, valLen, &ctx->databits, &ctx->parity,
                            &ctx->stopbits) < 0)
                return -EINVAL;
        } else {
            /* Platform addressing. Kept verbatim, in grant order. */
            size_t need = kvLen + (optsLen > 0 ? 1 : 0);
            if (optsLen + need >= sizeof(ctx->plat_opts))
                return -E2BIG;
            if (optsLen > 0)
                ctx->plat_opts[optsLen++] = ',';
            memcpy(ctx->plat_opts + optsLen, kv, kvLen);
            optsLen += kvLen;
            ctx->plat_opts[optsLen] = '\0';
        }
    }

    if (!have_port) {
        DEBUG_TRACE("uart grant has no port= clause");
        return -EINVAL;
    }
    return 0;
}

static void fillCfg(const struct vfs_driver_ctx_t *ctx, plat_uart_cfg_t *cfg) {
    cfg->port = ctx->port;
    cfg->options = ctx->plat_opts;
    cfg->baud = ctx->baud;
    cfg->databits = ctx->databits;
    cfg->parity = ctx->parity;
    cfg->stopbits = ctx->stopbits;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = UART_NODE_ROOT;
        return 0;
    }

    size_t portLen = strlen(ctx->port);
    if (strncmp(p, ctx->port, portLen) != 0)
        return -ENOENT;
    p += portLen;
    if (*p != '\0' && *p != '/')
        return -ENOENT;
    while (*p == '/')
        p++;

    if (*p == '\0')
        *node = UART_NODE_PORT;
    else if (strcmp(p, "data") == 0)
        *node = UART_NODE_DATA;
    else if (strcmp(p, "baud") == 0)
        *node = UART_NODE_BAUD;
    else if (strcmp(p, "format") == 0)
        *node = UART_NODE_FORMAT;
    else
        return -ENOENT;
    return 0;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

vfs_driver_t *VfsUartInit(const wapp_t *wapp, const char *options) {
    (void)wapp;

    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->ReadDir = _ReadDir;

    if (parseGrant(ctx, options) < 0) {
        _Destroy(driver);
        return NULL;
    }

    plat_uart_cfg_t cfg;
    fillCfg(ctx, &cfg);
    int rc = PlatformUartOpen(&cfg, &ctx->uart);
    if (rc < 0) {
        DEBUG_TRACE("uart %s: backing refused the port (%d)", ctx->port, rc);
        _Destroy(driver);
        return NULL;
    }

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        PlatformUartClose(ctx->uart);
        WantedFree(ctx);
    }
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    int rc = resolve(d, path != NULL ? path : "", &node);
    if (rc < 0)
        return rc;

    int slot = -1;
    for (int i = 0; i < UART_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EMFILE;

    d->fds[slot].used = true;
    d->fds[slot].node = node;
    d->fds[slot].nonblock = (flags & VFS_O_NONBLOCK) != 0;
    d->fds[slot].read_done = false;
    return slot;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= UART_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    if (fd < 0 || fd >= UART_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    uint8_t node = d->fds[fd].node;
    s->filetype = (node == UART_NODE_ROOT || node == UART_NODE_PORT)
                      ? VFS_FILETYPE_DIRECTORY
                      : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* Render the current setting of an attribute node into `line`. */
static size_t renderAttr(const struct vfs_driver_ctx_t *ctx, uint8_t node,
                         char *line, size_t lineLen) {
    if (node == UART_NODE_FORMAT) {
        if (lineLen < 5)
            return 0;
        line[0] = (char)('0' + ctx->databits);
        line[1] = (char)ctx->parity;
        line[2] = (char)('0' + ctx->stopbits);
        line[3] = '\n';
        return 4;
    }

    /* Decimal baud, built back to front. */
    char digits[10];
    size_t n = 0;
    uint32_t v = ctx->baud;
    do {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v != 0 && n < sizeof(digits));
    if (n + 1 > lineLen)
        return 0;
    for (size_t i = 0; i < n; i++)
        line[i] = digits[n - 1 - i];
    line[n] = '\n';
    return n + 1;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= UART_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct uart_fd_t *f = &d->fds[fd];
    if (f->node == UART_NODE_ROOT || f->node == UART_NODE_PORT)
        return -EISDIR;

    if (f->node != UART_NODE_DATA) {
        if (f->read_done)
            return 0;
        char line[UART_LINE_MAX];
        size_t len = renderAttr(d, f->node, line, sizeof(line));
        size_t n = (nbyte < len) ? nbyte : len;
        memcpy(buf, line, n);
        f->read_done = true;
        return (int)n;
    }

    if (nbyte == 0)
        return 0;

    /* Block until at least one byte arrives, and return short. Never wait to
     * fill the caller's buffer. */
    for (;;) {
        int n = PlatformUartRead(d->uart, buf, nbyte);
        if (n != 0)
            return n;
        if (f->nonblock)
            return -EAGAIN;
        /* A signalled stop interrupts the sleep. Return -EINTR so the read
         * unwinds to the interpreter, where the terminate flag is honoured. */
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC,
                                   UART_POLL_INTERVAL_NS, 0) == -EINTR)
            return -EINTR;
    }
}

/* Apply a new line configuration to the live port, then adopt it. */
static int applyCfg(struct vfs_driver_ctx_t *ctx, uint32_t baud,
                    uint8_t databits, uint8_t parity, uint8_t stopbits) {
    plat_uart_cfg_t cfg;
    cfg.port = ctx->port;
    cfg.options = ctx->plat_opts;
    cfg.baud = baud;
    cfg.databits = databits;
    cfg.parity = parity;
    cfg.stopbits = stopbits;

    int rc = PlatformUartConfigure(ctx->uart, &cfg);
    if (rc < 0)
        return rc;

    ctx->baud = baud;
    ctx->databits = databits;
    ctx->parity = parity;
    ctx->stopbits = stopbits;
    return 0;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= UART_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    const struct uart_fd_t *f = &d->fds[fd];
    if (f->node == UART_NODE_ROOT || f->node == UART_NODE_PORT)
        return -EISDIR;
    if (nbyte == 0)
        return 0;

    if (f->node == UART_NODE_BAUD || f->node == UART_NODE_FORMAT) {
        /* One line, trailing newline optional. */
        size_t len = nbyte;
        const char *p = buf;
        if (p[len - 1] == '\n')
            len--;

        if (f->node == UART_NODE_BAUD) {
            uint32_t baud = 0;
            if (parseDecimal(p, len, &baud) < 0 || baud == 0)
                return -EINVAL;
            int rc = applyCfg(d, baud, d->databits, d->parity, d->stopbits);
            if (rc < 0)
                return rc;
        } else {
            uint8_t databits;
            uint8_t parity;
            uint8_t stopbits;
            if (parseFormat(p, len, &databits, &parity, &stopbits) < 0)
                return -EINVAL;
            int rc = applyCfg(d, d->baud, databits, parity, stopbits);
            if (rc < 0)
                return rc;
        }
        return (int)nbyte;
    }

    for (;;) {
        int n = PlatformUartWrite(d->uart, buf, nbyte);
        if (n != 0)
            return n;
        if (f->nonblock)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC,
                                   UART_POLL_INTERVAL_NS, 0) == -EINTR)
            return -EINTR;
    }
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (fd < 0 || fd >= UART_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    const struct uart_fd_t *f = &d->fds[fd];

    if (f->node == UART_NODE_PORT) {
        static const vfs_dir_entry_t attrs[] = {
            {"data", VFS_FILETYPE_CHARACTER_DEVICE},
            {"baud", VFS_FILETYPE_CHARACTER_DEVICE},
            {"format", VFS_FILETYPE_CHARACTER_DEVICE},
        };
        return VfsFlatDirReadDir(attrs, 3, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != UART_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entry;
    entry.name = d->port;
    entry.type = VFS_FILETYPE_DIRECTORY;
    return VfsFlatDirReadDir(&entry, 1, buf, bufLen, cookie, bufUsed);
}
