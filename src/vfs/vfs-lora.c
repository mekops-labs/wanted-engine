/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <log-store.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-lora.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* LoRa driver: /dev/lora/<name>/{rx,tx,ctl,status}. One wapp owns a radio from
 * its first open of rx, tx or ctl; the core keeps the frame queue and enforces
 * the board's bands and duty cycle, whatever the backing does. */

static const char id[] = {'L', 'o', 'R', 'a'};

#define LORA_MAX_FDS 8
#define LORA_DEVICES_KEY "devices="
#define LORA_TX_TOKEN "tx"
#define LORA_SLEEP_NS 1000000ULL /* 1 ms */
#define LORA_CTL_MAX 160
#define LORA_STATUS_MAX 512
#define LORA_TOKENS_MAX 10
#define LORA_DEFAULT_SYNC 0x12
#define LORA_DEFAULT_CR 5
#define LORA_DEFAULT_PREAMBLE 8
#define LORA_HOUR_MS 3600000ULL
#define LORA_PERMILLE 1000ULL
#define LORA_US_PER_MS 1000U
#define LORA_NS_PER_MS 1000000ULL
#define LORA_HZ_PER_KHZ 1000U
/* Transmissions remembered per band; older ones merge, which overcounts. */
#define LORA_ACCOUNT_LEN 32

#define LORA_NODE_ROOT 0
#define LORA_NODE_DEVICE 1
#define LORA_NODE_RX 2
#define LORA_NODE_TX 3
#define LORA_NODE_CTL 4
#define LORA_NODE_STATUS 5

#define LORA_BYTE_BITS 8
#define LORA_BYTE_MASK 0xFFU

typedef enum {
    LORA_STATE_SLEEP,
    LORA_STATE_STANDBY,
    LORA_STATE_RX,
    LORA_STATE_TX,
} lora_state_t;

typedef struct lora_frame_t {
    uint8_t flags;
    uint8_t len;
    int16_t rssi;
    int16_t snr;
    uint64_t t_ms;
    uint8_t payload[LORA_PAYLOAD_MAX];
} lora_frame_t;

typedef struct lora_tx_record_t {
    uint64_t end_ms;
    uint32_t air_ms;
} lora_tx_record_t;

typedef struct lora_account_t {
    lora_tx_record_t rec[LORA_ACCOUNT_LEN];
    uint8_t head;
    uint8_t count;
} lora_account_t;

struct lora_device_t {
    char name[LORA_NAME_MAX + 1];
    const char *chip;
    const lora_ops_t *ops;
    void *ctx;
    lora_band_t bands[LORA_MAX_BANDS];
    lora_account_t accounts[LORA_MAX_BANDS];
    uint8_t bandCount;

    const void *owner; /* the driver instance holding the radio, or NULL */
    lora_state_t state;
    bool configured;
    bool cad;
    lora_params_t params;

    lora_frame_t q[CONFIG_WANTED_LORA_QUEUE_LEN];
    uint8_t head;
    uint8_t count;
    uint32_t pendingDropped;
    uint32_t rxFrames;
    uint32_t rxDropped;
    uint32_t txFrames;
    uint32_t txBusy;
    uint16_t lastLen; /* the last frame written, which tx_ready_ms is for */
};

static struct {
    lora_device_t devices[CONFIG_WANTED_LORA_MAX_DEVICES];
    uint8_t count;
    platform_mutex_t *lock;
} table;

struct lora_fd_t {
    bool used;
    uint8_t node;
    uint8_t device;
    bool nonblock;
    bool owns;
    uint32_t offset;
    char text[LORA_STATUS_MAX];
    uint32_t textLen;
};

struct vfs_driver_ctx_t {
    lora_device_t *devices[CONFIG_WANTED_LORA_MAX_DEVICES];
    uint8_t count;
    bool tx;
    int wakeFd;
    struct lora_fd_t fds[LORA_MAX_FDS];
};

/* ── Device table ────────────────────────────────────────────────────────── */

/* [A-Za-z0-9_-], 1..LORA_NAME_MAX characters. */
static bool validName(const char *s, size_t n) {
    if (n == 0 || n > LORA_NAME_MAX)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

static lora_device_t *findDevice(const char *name, size_t len) {
    for (uint8_t i = 0; i < table.count; i++) {
        if (strlen(table.devices[i].name) == len &&
            strncmp(table.devices[i].name, name, len) == 0)
            return &table.devices[i];
    }
    return NULL;
}

lora_device_t *LoraDeviceFind(const char *name) {
    return name != NULL ? findDevice(name, strlen(name)) : NULL;
}

int LoraDeviceRegister(const lora_device_desc_t *desc, lora_device_t **out) {
    if (desc == NULL || desc->name == NULL || desc->chip == NULL ||
        desc->ops == NULL || desc->ops->configure == NULL ||
        desc->ops->start == NULL || desc->ops->stop == NULL ||
        desc->ops->sleep == NULL || desc->band_count > LORA_MAX_BANDS ||
        (desc->band_count > 0 && desc->bands == NULL) ||
        !validName(desc->name, strlen(desc->name)))
        return -EINVAL;
    for (uint8_t i = 0; i < desc->band_count; i++) {
        const lora_band_t *b = &desc->bands[i];
        if (b->lo_hz >= b->hi_hz || b->duty_permille > LORA_PERMILLE)
            return -EINVAL;
    }
    if (findDevice(desc->name, strlen(desc->name)) != NULL)
        return -EEXIST;
    if (table.count >= CONFIG_WANTED_LORA_MAX_DEVICES)
        return -ENOSPC;

    lora_device_t *dev = &table.devices[table.count];
    memset(dev, 0, sizeof(*dev));
    strncpy(dev->name, desc->name, LORA_NAME_MAX);
    dev->chip = desc->chip;
    dev->ops = desc->ops;
    dev->ctx = desc->ctx;
    dev->bandCount = desc->band_count;
    if (desc->band_count > 0)
        memcpy(dev->bands, desc->bands, sizeof(lora_band_t) * desc->band_count);
    dev->state = LORA_STATE_SLEEP;
    dev->lastLen = LORA_PAYLOAD_MAX;
    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    table.count++;
    if (out != NULL)
        *out = dev;
    return 0;
}

void LoraDevicesReset(void) {
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

void LoraDevicePush(lora_device_t *dev, const lora_rx_frame_t *f) {
    if (dev == NULL || f == NULL || f->payload == NULL ||
        f->len > LORA_PAYLOAD_MAX || !f->crc_ok)
        return;

    PlatformMutexLock(table.lock);
    if (dev->owner != NULL && dev->state == LORA_STATE_RX) {
        if (dev->count >= CONFIG_WANTED_LORA_QUEUE_LEN) {
            dev->head =
                (uint8_t)((dev->head + 1) % CONFIG_WANTED_LORA_QUEUE_LEN);
            dev->count--;
            dev->pendingDropped++;
            dev->rxDropped++;
        }
        lora_frame_t *slot =
            &dev->q[(dev->head + dev->count) % CONFIG_WANTED_LORA_QUEUE_LEN];
        slot->flags = 1;
        slot->len = (uint8_t)f->len;
        slot->rssi = f->rssi_ddbm;
        slot->snr = f->snr_ddb;
        slot->t_ms = LogStoreUptimeMs();
        memcpy(slot->payload, f->payload, f->len);
        dev->count++;
        dev->rxFrames++;
    }
    PlatformMutexUnlock(table.lock);
}

/* ── Time on air ─────────────────────────────────────────────────────────── */

#define LORA_LDRO_SYMBOL_US 16000ULL
#define LORA_US_PER_S 1000000ULL

static uint64_t symbolUs(const lora_params_t *p) {
    return p->bw_hz == 0 ? 0 : (LORA_US_PER_S << p->sf) / p->bw_hz;
}

bool LoraLowDataRate(const lora_params_t *p) {
    return symbolUs(p) >= LORA_LDRO_SYMBOL_US;
}

/* Symbols in quarters: the preamble, 4.25 (6.25 at SF5 and SF6) and the
 * 8-symbol header block, then the coded payload in blocks of `cr` symbols. */
uint32_t LoraTimeOnAirUs(const lora_params_t *p, size_t len) {
    const int32_t sf = p->sf;
    const bool low = sf == 5 || sf == 6;
    const int32_t coeff1x4 = low ? 25 : 17;
    const int32_t coeff2 = low ? 0 : 8;
    const int32_t divisor = 4 * (LoraLowDataRate(p) ? sf - 2 : sf);
    /* 8 bits per byte, 16 of CRC, 20 of explicit header. */
    int32_t bits = (8 * (int32_t)len) + 16 - (4 * sf) + coeff2 + 20;
    if (bits < 0)
        bits = 0;
    uint64_t coded = (uint64_t)((bits + divisor - 1) / divisor);
    uint64_t symbolsX4 = (((uint64_t)p->preamble + 8) * 4) +
                         (uint64_t)coeff1x4 + (coded * p->cr * 4);
    return (uint32_t)(symbolUs(p) * symbolsX4 / 4);
}

/* ── Bands and the duty-cycle account ────────────────────────────────────── */

static uint64_t nowMs(void) {
    plat_timestamp_t ns = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &ns) < 0)
        return 0;
    return ns / LORA_NS_PER_MS;
}

/* The band holding the whole channel, freq ± bw / 2, or -1. */
static int bandFor(const lora_device_t *dev, uint32_t freq, uint32_t bw) {
    uint64_t lo = (uint64_t)freq - (bw / 2);
    uint64_t hi = (uint64_t)freq + (bw / 2);
    for (uint8_t i = 0; i < dev->bandCount; i++) {
        if (bw / 2 <= freq && lo >= dev->bands[i].lo_hz &&
            hi <= dev->bands[i].hi_hz)
            return i;
    }
    return -1;
}

static void accountExpire(lora_account_t *a, uint64_t now) {
    while (a->count > 0 && a->rec[a->head].end_ms + LORA_HOUR_MS <= now) {
        a->head = (uint8_t)((a->head + 1) % LORA_ACCOUNT_LEN);
        a->count--;
    }
}

static uint64_t accountUsed(const lora_account_t *a) {
    uint64_t used = 0;
    for (uint8_t i = 0; i < a->count; i++)
        used += a->rec[(a->head + i) % LORA_ACCOUNT_LEN].air_ms;
    return used;
}

/* Milliseconds until a frame of `air` fits the band's budget; 0 when it fits
 * now, UINT64_MAX when it never does. */
static uint64_t accountWait(lora_account_t *a, uint64_t budget, uint64_t air,
                            uint64_t now) {
    accountExpire(a, now);
    if (air > budget)
        return UINT64_MAX;
    uint64_t used = accountUsed(a);
    for (uint8_t i = 0; used + air > budget && i < a->count; i++) {
        const lora_tx_record_t *r = &a->rec[(a->head + i) % LORA_ACCOUNT_LEN];
        used -= r->air_ms;
        if (used + air <= budget)
            return r->end_ms + LORA_HOUR_MS - now;
    }
    return 0;
}

/* A full account merges its two oldest records into the later one. */
static void accountCharge(lora_account_t *a, uint64_t now, uint32_t air) {
    if (a->count == LORA_ACCOUNT_LEN) {
        uint8_t second = (uint8_t)((a->head + 1) % LORA_ACCOUNT_LEN);
        a->rec[second].air_ms += a->rec[a->head].air_ms;
        a->head = second;
        a->count--;
    }
    a->rec[(a->head + a->count) % LORA_ACCOUNT_LEN] =
        (lora_tx_record_t){now, air};
    a->count++;
}

static uint64_t bandBudgetMs(const lora_band_t *b) {
    return LORA_HOUR_MS * b->duty_permille / LORA_PERMILLE;
}

/* Air time of a frame in whole milliseconds, rounded up. */
static uint32_t frameAirMs(const lora_params_t *p, size_t len) {
    return (LoraTimeOnAirUs(p, len) + LORA_US_PER_MS - 1) / LORA_US_PER_MS;
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse devices=<name>[,<name>...][,tx] and resolve every name. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(LORA_DEVICES_KEY) - 1;
    if (options == NULL || strncmp(options, LORA_DEVICES_KEY, keyLen) != 0) {
        DEBUG_TRACE("lora grant has no devices= clause");
        return -EINVAL;
    }

    const char *p = options + keyLen;
    for (;;) {
        const char *end = p;
        while (*end != '\0' && *end != ',')
            end++;
        size_t len = (size_t)(end - p);

        if (len == 0 || ctx->tx)
            return -EINVAL;
        if (len == sizeof(LORA_TX_TOKEN) - 1 &&
            strncmp(p, LORA_TX_TOKEN, len) == 0) {
            ctx->tx = true;
        } else {
            lora_device_t *d = findDevice(p, len);
            if (d == NULL || ctx->count >= CONFIG_WANTED_LORA_MAX_DEVICES) {
                DEBUG_TRACE("lora grant names an unknown device");
                return -EINVAL;
            }
            for (uint8_t i = 0; i < ctx->count; i++) {
                if (ctx->devices[i] == d)
                    return -EINVAL;
            }
            ctx->devices[ctx->count++] = d;
        }
        if (*end == '\0')
            break;
        p = end + 1;
    }
    return ctx->count > 0 ? 0 : -EINVAL;
}

/* ── Ownership and power ─────────────────────────────────────────────────── */

static bool isOwnerNode(uint8_t node) {
    return node == LORA_NODE_RX || node == LORA_NODE_TX ||
           node == LORA_NODE_CTL;
}

/* Take the radio for `ctx`; the first claim wakes the chip into standby. */
static int claim(struct vfs_driver_ctx_t *ctx, lora_device_t *dev) {
    PlatformMutexLock(table.lock);
    if (dev->owner != NULL && dev->owner != ctx) {
        PlatformMutexUnlock(table.lock);
        return -EBUSY;
    }
    bool first = dev->owner == NULL;
    dev->owner = ctx;
    PlatformMutexUnlock(table.lock);
    if (!first)
        return 0;

    int rc = dev->ops->stop(dev->ctx);
    PlatformMutexLock(table.lock);
    if (rc < 0) {
        dev->owner = NULL;
        rc = -EIO;
    } else {
        dev->state = LORA_STATE_STANDBY;
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

/* When `ctx` closed its last owner descriptor, the chip sleeps and the next
 * owner starts from nothing. */
static void release(struct vfs_driver_ctx_t *ctx, lora_device_t *dev) {
    for (int i = 0; i < LORA_MAX_FDS; i++) {
        const struct lora_fd_t *f = &ctx->fds[i];
        if (f->used && f->owns && ctx->devices[f->device] == dev)
            return;
    }
    if (dev->owner != ctx)
        return;
    (void)dev->ops->sleep(dev->ctx);
    PlatformMutexLock(table.lock);
    dev->owner = NULL;
    dev->state = LORA_STATE_SLEEP;
    dev->configured = false;
    dev->cad = false;
    dev->head = 0;
    dev->count = 0;
    dev->pendingDropped = 0;
    PlatformMutexUnlock(table.lock);
}

static void setState(lora_device_t *dev, lora_state_t s) {
    PlatformMutexLock(table.lock);
    dev->state = s;
    PlatformMutexUnlock(table.lock);
}

/* ── ctl ─────────────────────────────────────────────────────────────────── */

static bool parseU32(const char *s, uint32_t *out) {
    uint64_t v = 0;
    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9')
            return false;
        v = (v * 10) + (uint64_t)(*s - '0');
        if (v > UINT32_MAX)
            return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool parseI32(const char *s, int32_t *out) {
    bool neg = *s == '-';
    uint32_t v = 0;
    if (!parseU32(neg ? s + 1 : s, &v) || v > INT32_MAX)
        return false;
    *out = neg ? -(int32_t)v : (int32_t)v;
    return true;
}

/* kHz with up to three decimals, such as 125, 62.5 or 31.25, into Hz. */
static bool parseKhz(const char *s, uint32_t *hz) {
    uint64_t v = 0;
    int decimals = -1;
    if (*s == '\0')
        return false;
    for (; *s != '\0'; s++) {
        if (*s == '.' && decimals < 0) {
            decimals = 0;
            continue;
        }
        if (*s < '0' || *s > '9' || decimals >= 3)
            return false;
        v = (v * 10) + (uint64_t)(*s - '0');
        if (decimals >= 0)
            decimals++;
        if (v > UINT32_MAX)
            return false;
    }
    for (int d = decimals < 0 ? 0 : decimals; d < 3; d++)
        v *= 10;
    if (v == 0 || v > UINT32_MAX)
        return false;
    *hz = (uint32_t)v;
    return true;
}

/* `0x` and two hex digits. */
static bool parseSync(const char *s, uint8_t *out) {
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || s[2] == '\0' ||
        strlen(s) > 4)
        return false;
    unsigned v = 0;
    for (const char *p = s + 2; *p != '\0'; p++) {
        char c = *p;
        unsigned d;
        if (c >= '0' && c <= '9')
            d = (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = (unsigned)(c - 'A' + 10);
        else
            return false;
        v = (v * 16) + d;
    }
    *out = (uint8_t)v;
    return true;
}

/* `set freq= sf= bw= [cr=] [sync=] [power=] [preamble=] [cad=]`. Applied
 * whole: the chip and the state are left alone on any error. */
static int ctlSet(lora_device_t *dev, char **tok, int ntok) {
    static const char *const keys[] = {"freq", "sf",    "bw",       "cr",
                                       "sync", "power", "preamble", "cad"};
    lora_params_t p = {0};
    uint32_t seen = 0;
    bool cad = false;
    int32_t power = 0;
    uint32_t v = 0;

    p.cr = LORA_DEFAULT_CR;
    p.sync = LORA_DEFAULT_SYNC;
    p.preamble = LORA_DEFAULT_PREAMBLE;
    for (int i = 1; i < ntok; i++) {
        char *eq = strchr(tok[i], '=');
        if (eq == NULL)
            return -EINVAL;
        *eq = '\0';
        const char *val = eq + 1;
        int k = -1;
        for (int j = 0; j < (int)(sizeof(keys) / sizeof(keys[0])); j++) {
            if (strcmp(tok[i], keys[j]) == 0)
                k = j;
        }
        if (k < 0 || (seen & (1U << k)) != 0)
            return -EINVAL;
        seen |= 1U << k;

        bool ok;
        switch (k) {
        case 0:
            ok = parseU32(val, &v) && v > 0;
            p.freq_hz = v;
            break;
        case 1:
            ok = parseU32(val, &v) && v >= 5 && v <= 12;
            p.sf = (uint8_t)v;
            break;
        case 2:
            ok = parseKhz(val, &p.bw_hz);
            break;
        case 3:
            ok = parseU32(val, &v) && v >= 5 && v <= 8;
            p.cr = (uint8_t)v;
            break;
        case 4:
            ok = parseSync(val, &p.sync);
            break;
        case 5:
            ok =
                parseI32(val, &power) && power >= INT8_MIN && power <= INT8_MAX;
            break;
        case 6:
            ok = parseU32(val, &v) && v >= 1 && v <= UINT16_MAX;
            p.preamble = (uint16_t)v;
            break;
        default:
            ok = strcmp(val, "0") == 0 || strcmp(val, "1") == 0;
            cad = val[0] == '1';
            break;
        }
        if (!ok)
            return -EINVAL;
    }
    /* freq, sf and bw are required. */
    if ((seen & 0x7U) != 0x7U)
        return -EINVAL;
    if (cad && dev->ops->channel_active == NULL)
        return -ENOTSUP;

    /* Receive is never limited: a channel outside the bands only cannot send.
     */
    int band = bandFor(dev, p.freq_hz, p.bw_hz);
    int32_t limit = band >= 0 ? dev->bands[band].max_dbm : INT8_MAX;
    if ((seen & (1U << 5)) == 0)
        power = band >= 0 ? limit : 0;
    p.power_dbm = (int8_t)(power > limit ? limit : power);

    /* A receiving radio stops for the change and resumes on the result. */
    bool receiving = dev->state == LORA_STATE_RX;
    if (receiving && dev->ops->stop(dev->ctx) < 0)
        return -EIO;
    int rc = dev->ops->configure(dev->ctx, &p);
    if (rc == 0) {
        PlatformMutexLock(table.lock);
        dev->params = p;
        dev->configured = true;
        dev->cad = cad;
        PlatformMutexUnlock(table.lock);
    }
    if (receiving && dev->ops->start(dev->ctx) < 0) {
        setState(dev, LORA_STATE_STANDBY);
        return -EIO;
    }
    if (rc == -EINVAL)
        return -EINVAL;
    return rc < 0 ? -EIO : 0;
}

static int writeCtl(lora_device_t *dev, const void *buf, size_t nbyte) {
    char line[LORA_CTL_MAX];
    if (nbyte == 0 || nbyte >= sizeof(line))
        return -EINVAL;
    memcpy(line, buf, nbyte);
    line[nbyte] = '\0';
    if (line[nbyte - 1] == '\n')
        line[nbyte - 1] = '\0';

    for (size_t i = 0; line[i] != '\0'; i++) {
        if (line[i] == '\n' || line[i] == '\t' || line[i] == '\r')
            return -EINVAL;
    }

    char *tok[LORA_TOKENS_MAX];
    int ntok = 0;
    char *p = line;
    while (*p != '\0') {
        if (*p == ' ') {
            p++;
            continue;
        }
        if (ntok == LORA_TOKENS_MAX)
            return -EINVAL;
        tok[ntok++] = p;
        while (*p != '\0' && *p != ' ')
            p++;
        if (*p == ' ')
            *p++ = '\0';
    }
    if (ntok == 0)
        return -EINVAL;

    int rc;
    if (strcmp(tok[0], "set") == 0) {
        rc = ctlSet(dev, tok, ntok);
    } else if (strcmp(tok[0], "start") == 0 && ntok == 1) {
        if (!dev->configured)
            return -EINVAL;
        rc = dev->ops->start(dev->ctx) < 0 ? -EIO : 0;
        if (rc == 0)
            setState(dev, LORA_STATE_RX);
    } else if (strcmp(tok[0], "stop") == 0 && ntok == 1) {
        rc = dev->ops->stop(dev->ctx) < 0 ? -EIO : 0;
        if (rc == 0)
            setState(dev, LORA_STATE_STANDBY);
    } else {
        rc = -EINVAL;
    }
    return rc < 0 ? rc : (int)nbyte;
}

/* ── tx ──────────────────────────────────────────────────────────────────── */

static int writeTx(lora_device_t *dev, const void *buf, size_t nbyte) {
    if (nbyte == 0)
        return -EINVAL;
    if (nbyte > LORA_PAYLOAD_MAX)
        return -EMSGSIZE;
    if (dev->ops->transmit == NULL)
        return -ENOTSUP;
    if (!dev->configured)
        return -EINVAL;
    int band = bandFor(dev, dev->params.freq_hz, dev->params.bw_hz);
    if (band < 0)
        return -EPERM;

    uint32_t air = frameAirMs(&dev->params, nbyte);
    lora_account_t *acct = &dev->accounts[band];
    PlatformMutexLock(table.lock);
    dev->lastLen = (uint16_t)nbyte;
    uint64_t wait =
        accountWait(acct, bandBudgetMs(&dev->bands[band]), air, nowMs());
    PlatformMutexUnlock(table.lock);
    if (wait > 0)
        return -EAGAIN;

    lora_state_t before = dev->state;
    if (before == LORA_STATE_RX && dev->ops->stop(dev->ctx) < 0)
        return -EIO;
    if (dev->cad) {
        bool active = false;
        int rc = dev->ops->channel_active(dev->ctx, &active);
        if (rc < 0 || active) {
            if (before == LORA_STATE_RX)
                (void)dev->ops->start(dev->ctx);
            if (rc < 0)
                return -EIO;
            PlatformMutexLock(table.lock);
            dev->txBusy++;
            PlatformMutexUnlock(table.lock);
            return -EBUSY;
        }
    }

    setState(dev, LORA_STATE_TX);
    int rc = dev->ops->transmit(dev->ctx, buf, nbyte);
    PlatformMutexLock(table.lock);
    if (rc == 0) {
        accountCharge(acct, nowMs(), air);
        dev->txFrames++;
    }
    dev->state = LORA_STATE_STANDBY;
    PlatformMutexUnlock(table.lock);
    if (before == LORA_STATE_RX) {
        if (dev->ops->start(dev->ctx) < 0)
            return -EIO;
        setState(dev, LORA_STATE_RX);
    }
    return rc < 0 ? -EIO : (int)nbyte;
}

/* ── status ──────────────────────────────────────────────────────────────── */

static const char *stateName(lora_state_t s) {
    switch (s) {
    case LORA_STATE_STANDBY:
        return "standby";
    case LORA_STATE_RX:
        return "rx";
    case LORA_STATE_TX:
        return "tx";
    default:
        return "sleep";
    }
}

/* tx_ready_ms for the applied channel and the last frame size; 0 without a
 * channel, a full 255 bytes before the first frame. */
static uint64_t readyMs(lora_device_t *dev) {
    if (!dev->configured)
        return 0;
    int band = bandFor(dev, dev->params.freq_hz, dev->params.bw_hz);
    if (band < 0)
        return 0;
    uint64_t w =
        accountWait(&dev->accounts[band], bandBudgetMs(&dev->bands[band]),
                    frameAirMs(&dev->params, dev->lastLen), nowMs());
    return w == UINT64_MAX ? LORA_HOUR_MS : w;
}

static uint32_t renderStatus(lora_device_t *dev, char *out, size_t cap) {
    PlatformMutexLock(table.lock);
    int n = snprintf(out, cap, "state=%s\nowner=%s\n", stateName(dev->state),
                     dev->owner != NULL ? "held" : "none");
    if (dev->configured && n >= 0 && (size_t)n < cap) {
        const lora_params_t *p = &dev->params;
        n += snprintf(out + n, cap - (size_t)n,
                      "freq=%lu\nsf=%u\nbw=%lu.%03lu\ncr=%u\nsync=0x%02X\n"
                      "power=%d\npreamble=%u\ncad=%d\n",
                      (unsigned long)p->freq_hz, (unsigned)p->sf,
                      (unsigned long)(p->bw_hz / LORA_HZ_PER_KHZ),
                      (unsigned long)(p->bw_hz % LORA_HZ_PER_KHZ),
                      (unsigned)p->cr, (unsigned)p->sync, (int)p->power_dbm,
                      (unsigned)p->preamble, dev->cad ? 1 : 0);
    }
    if (n >= 0 && (size_t)n < cap) {
        n += snprintf(out + n, cap - (size_t)n,
                      "rx_frames=%lu\nrx_dropped=%lu\nqueue=%u\n"
                      "tx_frames=%lu\ntx_busy=%lu\ntx_ready_ms=%llu\n"
                      "chip=%s\n",
                      (unsigned long)dev->rxFrames,
                      (unsigned long)dev->rxDropped, (unsigned)dev->count,
                      (unsigned long)dev->txFrames, (unsigned long)dev->txBusy,
                      (unsigned long long)readyMs(dev), dev->chip);
    }
    PlatformMutexUnlock(table.lock);
    if (n < 0)
        return 0;
    return (size_t)n < cap ? (uint32_t)n : (uint32_t)(cap - 1);
}

/* ── rx ──────────────────────────────────────────────────────────────────── */

static void putU16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & LORA_BYTE_MASK);
    p[1] = (uint8_t)(v >> LORA_BYTE_BITS);
}

static void putU32(uint8_t *p, uint32_t v) {
    for (size_t i = 0; i < sizeof(v); i++)
        p[i] = (uint8_t)((v >> (LORA_BYTE_BITS * i)) & LORA_BYTE_MASK);
}

static void putU64(uint8_t *p, uint64_t v) {
    for (size_t i = 0; i < sizeof(v); i++)
        p[i] = (uint8_t)((v >> (LORA_BYTE_BITS * i)) & LORA_BYTE_MASK);
}

/* Header: flags, reserved, len u16, rssi i16, snr i16, dropped u32, t_ms u64.
 */
static size_t putRecord(uint8_t *p, const lora_frame_t *fr, uint32_t dropped) {
    p[0] = fr->flags;
    p[1] = 0;
    putU16(p + 2, fr->len);
    putU16(p + 4, (uint16_t)fr->rssi);
    putU16(p + 6, (uint16_t)fr->snr);
    putU32(p + 8, dropped);
    putU64(p + 12, fr->t_ms);
    memcpy(p + LORA_RECORD_HEADER, fr->payload, fr->len);
    return LORA_RECORD_HEADER + fr->len;
}

/* Whole records into `buf`: 0 when none is queued, -EINVAL when the next one
 * does not fit. */
static int popRecords(lora_device_t *dev, uint8_t *buf, size_t nbyte) {
    size_t used = 0;
    int rc = 0;
    PlatformMutexLock(table.lock);
    while (dev->count > 0) {
        const lora_frame_t *fr = &dev->q[dev->head];
        if (used + LORA_RECORD_HEADER + fr->len > nbyte) {
            if (used == 0)
                rc = -EINVAL;
            break;
        }
        used += putRecord(buf + used, fr, dev->pendingDropped);
        dev->pendingDropped = 0;
        dev->head = (uint8_t)((dev->head + 1) % CONFIG_WANTED_LORA_QUEUE_LEN);
        dev->count--;
    }
    PlatformMutexUnlock(table.lock);
    return rc < 0 ? rc : (int)used;
}

/* Blocks until a record is queued; a stop ends the wait. */
static int readRx(vfs_driver_ctx_t d, const struct lora_fd_t *f, void *buf,
                  size_t nbyte) {
    for (;;) {
        int n = popRecords(d->devices[f->device], buf, nbyte);
        if (n != 0)
            return n;
        if (f->nonblock)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, LORA_SLEEP_NS, 0) ==
                -EINTR ||
            PlatformWakeRaised(d->wakeFd))
            return -EINTR;
    }
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *device) {
    const char *p = path;
    while (*p == '/')
        p++;
    *device = 0;
    if (*p == '\0') {
        *node = LORA_NODE_ROOT;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;
    int idx = -1;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strlen(ctx->devices[i]->name) == (size_t)(slash - p) &&
            strncmp(ctx->devices[i]->name, p, (size_t)(slash - p)) == 0)
            idx = i;
    }
    if (idx < 0)
        return -ENOENT;
    *device = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0')
        *node = LORA_NODE_DEVICE;
    else if (strcmp(slash, "rx") == 0)
        *node = LORA_NODE_RX;
    else if (strcmp(slash, "tx") == 0)
        *node = LORA_NODE_TX;
    else if (strcmp(slash, "ctl") == 0)
        *node = LORA_NODE_CTL;
    else if (strcmp(slash, "status") == 0)
        *node = LORA_NODE_STATUS;
    else
        return -ENOENT;
    return 0;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);
static void _SetWake(vfs_driver_ctx_t d, int fd);
static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags);
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsLoraInit(const wapp_t *wapp, const char *options) {
    (void)wapp;

    vfs_driver_t *driver = WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }
    struct vfs_driver_ctx_t *ctx = WantedMalloc(sizeof(*ctx));
    if (ctx == NULL) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->wakeFd = -1;
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
    driver->Seek = _Seek;
    driver->SetWake = _SetWake;
    driver->SetFlags = _SetFlags;
    driver->Poll = _Poll;
    driver->ReadDir = _ReadDir;

    if (parseGrant(ctx, options) < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        for (int i = 0; i < LORA_MAX_FDS; i++)
            ctx->fds[i].used = false;
        for (uint8_t i = 0; i < ctx->count; i++)
            release(ctx, ctx->devices[i]);
    }
    WantedFree(ctx);
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct lora_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= LORA_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    uint8_t device;
    int rc = resolve(d, path != NULL ? path : "", &node, &device);
    if (rc < 0)
        return rc;
    if (node == LORA_NODE_TX && !d->tx && VFS_O_IS_WRITE(flags))
        return -EACCES;

    int slot = -1;
    for (int i = 0; i < LORA_MAX_FDS && slot < 0; i++) {
        if (!d->fds[i].used)
            slot = i;
    }
    if (slot < 0)
        return -EMFILE;
    if (isOwnerNode(node)) {
        rc = claim(d, d->devices[device]);
        if (rc < 0)
            return rc;
    }

    struct lora_fd_t *f = &d->fds[slot];
    memset(f, 0, sizeof(*f));
    f->used = true;
    f->node = node;
    f->device = device;
    f->owns = isOwnerNode(node);
    f->nonblock = (flags & VFS_O_NONBLOCK) != 0;
    if (node == LORA_NODE_STATUS)
        f->textLen = renderStatus(d->devices[device], f->text, sizeof(f->text));
    return slot;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    bool owns = f->owns;
    lora_device_t *dev = d->devices[f->device];
    memset(f, 0, sizeof(*f));
    if (owns)
        release(d, dev);
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == LORA_NODE_ROOT || f->node == LORA_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static void _SetWake(vfs_driver_ctx_t d, int fd) { d->wakeFd = fd; }

static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags) {
    struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    f->nonblock = (flags & VFS_O_NONBLOCK) != 0;
    return 0;
}

/* rx waits for a frame; every other node is always ready. */
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail) {
    const struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == LORA_NODE_RX) {
        const lora_device_t *dev = d->devices[f->device];
        PlatformMutexLock(table.lock);
        uint32_t bytes =
            dev->count > 0 ? LORA_RECORD_HEADER + dev->q[dev->head].len : 0;
        PlatformMutexUnlock(table.lock);
        *avail = bytes;
        return bytes > 0 ? VFS_POLL_IN : 0;
    }
    return VFS_POLL_IN | VFS_POLL_OUT;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case LORA_NODE_RX:
        return readRx(d, f, buf, nbyte);
    case LORA_NODE_STATUS: {
        if (f->offset >= f->textLen)
            return 0;
        size_t n = f->textLen - f->offset;
        if (n > nbyte)
            n = nbyte;
        memcpy(buf, f->text + f->offset, n);
        f->offset += (uint32_t)n;
        return (int)n;
    }
    case LORA_NODE_ROOT:
    case LORA_NODE_DEVICE:
        return -EISDIR;
    default:
        return -EPERM;
    }
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case LORA_NODE_CTL:
        return writeCtl(d->devices[f->device], buf, nbyte);
    case LORA_NODE_TX:
        if (!d->tx)
            return -EACCES;
        return writeTx(d->devices[f->device], buf, nbyte);
    case LORA_NODE_ROOT:
    case LORA_NODE_DEVICE:
        return -EISDIR;
    default:
        return -EPERM;
    }
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 /* NOLINTNEXTLINE(readability-non-const-parameter) */
                 long *pos) {
    (void)off;
    (void)whence;
    (void)pos;
    return fdAt(d, fd) == NULL ? -EBADF : -ESPIPE;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct lora_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == LORA_NODE_DEVICE) {
        vfs_dir_entry_t nodes[4] = {
            {"rx", VFS_FILETYPE_CHARACTER_DEVICE},
            {"tx", VFS_FILETYPE_CHARACTER_DEVICE},
            {"ctl", VFS_FILETYPE_CHARACTER_DEVICE},
            {"status", VFS_FILETYPE_CHARACTER_DEVICE},
        };
        return VfsFlatDirReadDir(nodes, 4, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != LORA_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_LORA_MAX_DEVICES];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->devices[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
