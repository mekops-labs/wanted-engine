/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-wbus.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>
#include <wbus.h>

/* The local message bus behind /dev/wbus/{msg,status}. One store holds the
 * message slots, endpoints, pending calls and objects under one lock. A blocked
 * call sleeps on its wapp's ready event, so nothing polls. */

#define SLOT_SIZE (WBUS_HEADER_SIZE + CONFIG_WANTED_WBUS_PAYLOAD_MAX)
#define EP_MAX                                                                 \
    (CONFIG_WANTED_WBUS_ENDPOINTS > 0 ? CONFIG_WANTED_WBUS_ENDPOINTS           \
                                      : CONFIG_WANTED_MAX_WAPPS + 2)
/* Replies ride in reserved slots, so they never count against the queue. A
 * call stays pending until its reply is read, which bounds the replies. */
#define QUEUE_CAP (CONFIG_WANTED_WBUS_QUEUE + CONFIG_WANTED_WBUS_PENDING)
#define CALL_MAX ((size_t)EP_MAX * CONFIG_WANTED_WBUS_PENDING)
#define HANDLE_MAX (CONFIG_WANTED_WBUS_EP_PER_WAPP + 2)
#define STATUS_CAP 512
#define NO_SLOT 0xFFFFu
#define NO_EP (-1)
/* Internal result: the write may wait for room. */
#define WAIT (-100000)
#define RECHECK_NS 1000000ULL /* without a ready event, a wait rechecks */

_Static_assert(WAPP_MAX_NAME_LEN <= WBUS_NAME_SIZE,
               "an instance name fits the peer field");

typedef enum { CL_SERVE, CL_CALL, CL_LISTEN } clause_kind_t;

typedef struct clause_t {
    clause_kind_t kind;
    char object[WBUS_NAME_SIZE];
    char member[WBUS_NAME_SIZE]; /* "*" for every member; empty for serve */
} clause_t;

typedef struct sub_t {
    char object[WBUS_NAME_SIZE]; /* empty: the entry is free */
    char member[WBUS_NAME_SIZE];
} sub_t;

typedef struct ep_t {
    bool used;
    struct wbus_wapp_t *wapp;
    uint16_t q[QUEUE_CAP];
    uint8_t head;
    uint8_t cnt;
    uint8_t qOther;      /* queued messages that are not replies */
    uint8_t pending;     /* calls sent whose reply is not yet read */
    uint16_t dropped;    /* events lost since the previous read */
    uint32_t droppedAll; /* events lost since the endpoint opened */
    sub_t subs[CONFIG_WANTED_WBUS_SUBS];
} ep_t;

typedef struct call_t {
    bool used;
    uint32_t engId;
    uint32_t clientId;
    int client; /* NO_EP once the client is gone */
    int server;
    uint16_t reqSlot; /* NO_SLOT once the server has read it */
    uint16_t repSlot; /* reserved for the reply; NO_SLOT without a client */
    char object[WBUS_NAME_SIZE];
    char member[WBUS_NAME_SIZE];
} call_t;

typedef struct object_t {
    char name[WBUS_NAME_SIZE]; /* empty: the entry is free */
    int ep;
} object_t;

struct wbus_store_t {
    platform_mutex_t *lock;
    uint8_t *pool;
    uint16_t next[CONFIG_WANTED_WBUS_POOL];
    uint16_t freeHead;
    uint16_t freeCnt;
    ep_t eps[EP_MAX];
    call_t calls[CALL_MAX];
    object_t objects[CONFIG_WANTED_WBUS_OBJECTS];
    uint32_t nextId;
    /* Ready events of the wapps waiting for pool space, one entry each. */
    plat_ready_t *poolWaiters[EP_MAX];
    size_t poolWaitCnt;
};

typedef enum { H_ROOT, H_MSG, H_STATUS } handle_kind_t;

typedef struct wbus_handle_t {
    handle_kind_t kind;
    int ep;
    vfs_oflags_t flags;
    size_t pos;
    size_t len;
    char text[]; /* a status snapshot; empty room otherwise */
} wbus_handle_t;

/* One wapp's driver: its grant and the descriptors it holds. */
typedef struct wbus_wapp_t {
    wbus_store_t *store;
    char name[WBUS_NAME_SIZE];
    clause_t clauses[CONFIG_WANTED_WBUS_CLAUSES];
    size_t clauseCnt;
    size_t epCnt;
    plat_ready_t *ready;
    wbus_handle_t *handles[HANDLE_MAX];
} wbus_wapp_t;

/* ── Wire encoding ───────────────────────────────────────────────────────────
 */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v) {
    wr16(p, (uint16_t)v);
    wr16(p + 2, (uint16_t)(v >> 16));
}

static void hdrGet(wbus_header_t *h, const uint8_t *b) {
    h->version = b[0];
    h->type = b[1];
    h->len = rd16(b + 2);
    h->id = rd32(b + 4);
    h->status = rd32(b + 8);
    h->dropped = rd16(b + 12);
    h->reserved = rd16(b + 14);
    memcpy(h->object, b + 16, WBUS_NAME_SIZE);
    memcpy(h->member, b + 32, WBUS_NAME_SIZE);
    memcpy(h->peer, b + 48, WBUS_NAME_SIZE);
}

static void hdrPut(uint8_t *b, const wbus_header_t *h) {
    b[0] = h->version;
    b[1] = h->type;
    wr16(b + 2, h->len);
    wr32(b + 4, h->id);
    wr32(b + 8, h->status);
    wr16(b + 12, h->dropped);
    wr16(b + 14, h->reserved);
    memcpy(b + 16, h->object, WBUS_NAME_SIZE);
    memcpy(b + 32, h->member, WBUS_NAME_SIZE);
    memcpy(b + 48, h->peer, WBUS_NAME_SIZE);
}

/* ── Names and grants ────────────────────────────────────────────────────────
 */

static bool nameChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/* Length of the NUL-padded name in `f`; -1 when a character is not allowed,
 * no NUL ends it, or bytes follow the NUL. 0 is an empty field. */
static int fieldLen(const char *f) {
    size_t n = 0;
    while (n < WBUS_NAME_SIZE && f[n] != '\0') {
        if (!nameChar(f[n]))
            return -1;
        n++;
    }
    if (n == WBUS_NAME_SIZE)
        return -1;
    for (size_t i = n; i < WBUS_NAME_SIZE; i++) {
        if (f[i] != '\0')
            return -1;
    }
    return (int)n;
}

/* The reserved object, padded as a name field is. */
static const char supervisorName[WBUS_NAME_SIZE] = WBUS_SUPERVISOR;

static bool fieldEq(const char *a, const char *b) {
    return memcmp(a, b, WBUS_NAME_SIZE) == 0;
}

/* Store the `n`-byte name `s` NUL-padded. False when it is not a name; with
 * `star`, the single character `*` is one. */
static bool fieldSet(char *f, const char *s, size_t n, bool star) {
    if (n == 0 || n > WBUS_NAME_MAX)
        return false;
    if (!(star && n == 1 && s[0] == '*')) {
        for (size_t i = 0; i < n; i++) {
            if (!nameChar(s[i]))
                return false;
        }
    }
    memset(f, 0, WBUS_NAME_SIZE);
    memcpy(f, s, n);
    return true;
}

/* Parse one "<object>" or "<object>.<member>" after its "key=". */
static int clauseParse(clause_t *c, const char *s, size_t n) {
    if (c->kind == CL_SERVE) {
        memset(c->member, 0, WBUS_NAME_SIZE);
        return fieldSet(c->object, s, n, false) ? 0 : -EINVAL;
    }
    const char *dot = memchr(s, '.', n);
    if (dot == NULL)
        return -EINVAL;
    size_t on = (size_t)(dot - s);
    if (!fieldSet(c->object, s, on, false) ||
        !fieldSet(c->member, dot + 1, n - on - 1, true))
        return -EINVAL;
    return 0;
}

static int grantParse(wbus_wapp_t *w, const char *grant) {
    if (grant == NULL || grant[0] == '\0')
        return -EINVAL;
    const char *p = grant;
    while (*p != '\0') {
        size_t n = strcspn(p, ",");
        clause_t c;
        memset(&c, 0, sizeof(c));
        size_t key;
        if (n > 6 && strncmp(p, "serve=", 6) == 0) {
            c.kind = CL_SERVE;
            key = 6;
        } else if (n > 5 && strncmp(p, "call=", 5) == 0) {
            c.kind = CL_CALL;
            key = 5;
        } else if (n > 7 && strncmp(p, "listen=", 7) == 0) {
            c.kind = CL_LISTEN;
            key = 7;
        } else {
            return -EINVAL;
        }
        int rc = clauseParse(&c, p + key, n - key);
        if (rc < 0)
            return rc;
        if (c.kind == CL_SERVE && fieldEq(c.object, supervisorName) &&
            !fieldEq(w->name, supervisorName))
            return -EPERM;
        if (w->clauseCnt >= CONFIG_WANTED_WBUS_CLAUSES)
            return -ENOSPC;
        w->clauses[w->clauseCnt++] = c;
        p += n;
        if (*p == ',') {
            p++;
            if (*p == '\0')
                return -EINVAL;
        }
    }
    return 0;
}

/* Whether a clause of `kind` covers `object` and, unless it is a serve clause,
 * `member`. */
static bool allowed(const wbus_wapp_t *w, clause_kind_t kind,
                    const char *object, const char *member) {
    static const char star[WBUS_NAME_SIZE] = "*";
    for (size_t i = 0; i < w->clauseCnt; i++) {
        const clause_t *c = &w->clauses[i];
        if (c->kind != kind || !fieldEq(c->object, object))
            continue;
        if (kind == CL_SERVE || fieldEq(c->member, star) ||
            fieldEq(c->member, member))
            return true;
    }
    return false;
}

/* ── Pool and queues (all under the store lock) ──────────────────────────────
 */

static uint8_t *slotPtr(const wbus_store_t *st, uint16_t i) {
    return st->pool + ((size_t)i * SLOT_SIZE);
}

static void poolRaise(const wbus_store_t *st) {
    for (size_t i = 0; i < st->poolWaitCnt; i++)
        PlatformReadyRaise(st->poolWaiters[i]);
}

static void poolWaitAdd(wbus_store_t *st, plat_ready_t *r) {
    if (r == NULL)
        return;
    for (size_t i = 0; i < st->poolWaitCnt; i++) {
        if (st->poolWaiters[i] == r)
            return;
    }
    if (st->poolWaitCnt < EP_MAX)
        st->poolWaiters[st->poolWaitCnt++] = r;
}

static void poolWaitDel(wbus_store_t *st, const plat_ready_t *r) {
    for (size_t i = 0; i < st->poolWaitCnt; i++) {
        if (st->poolWaiters[i] != r)
            continue;
        st->poolWaiters[i] = st->poolWaiters[--st->poolWaitCnt];
        return;
    }
}

static uint16_t slotAlloc(wbus_store_t *st) {
    uint16_t i = st->freeHead;
    if (i != NO_SLOT) {
        st->freeHead = st->next[i];
        st->freeCnt--;
    }
    return i;
}

static void slotFree(wbus_store_t *st, uint16_t i) {
    st->next[i] = st->freeHead;
    st->freeHead = i;
    st->freeCnt++;
    poolRaise(st);
}

static bool isReply(const wbus_store_t *st, uint16_t slot) {
    return slotPtr(st, slot)[1] == WBUS_REPLY;
}

static void qPush(const wbus_store_t *st, ep_t *e, uint16_t slot) {
    e->q[(e->head + e->cnt) % QUEUE_CAP] = slot;
    e->cnt++;
    if (!isReply(st, slot))
        e->qOther++;
}

static uint16_t qPop(const wbus_store_t *st, ep_t *e) {
    uint16_t slot = e->q[e->head];
    e->head = (uint8_t)((e->head + 1) % QUEUE_CAP);
    e->cnt--;
    if (!isReply(st, slot))
        e->qOther--;
    return slot;
}

static void qRemove(const wbus_store_t *st, ep_t *e, uint16_t slot) {
    uint8_t n = e->cnt;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t s = qPop(st, e);
        if (s != slot)
            qPush(st, e, s);
    }
}

/* Queue `slot` for `e` and wake its wapp. */
static void deliver(const wbus_store_t *st, ep_t *e, uint16_t slot) {
    qPush(st, e, slot);
    PlatformReadyRaise(e->wapp->ready);
}

static int objectFind(const wbus_store_t *st, const char *name) {
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_OBJECTS; i++) {
        if (fieldEq(st->objects[i].name, name))
            return (int)i;
    }
    return -1;
}

static call_t *callFind(wbus_store_t *st, uint32_t engId) {
    for (size_t i = 0; i < CALL_MAX; i++) {
        if (st->calls[i].used && st->calls[i].engId == engId)
            return &st->calls[i];
    }
    return NULL;
}

/* Fill a reserved reply slot and queue it to the call's client. */
static void replyTo(wbus_store_t *st, const call_t *c, const char *server,
                    uint32_t status, const uint8_t *payload, uint16_t len) {
    wbus_header_t h;
    memset(&h, 0, sizeof(h));
    h.version = WBUS_VERSION;
    h.type = WBUS_REPLY;
    h.len = len;
    h.id = c->clientId;
    h.status = status;
    memcpy(h.object, c->object, WBUS_NAME_SIZE);
    memcpy(h.member, c->member, WBUS_NAME_SIZE);
    memcpy(h.peer, server, WBUS_NAME_SIZE);
    uint8_t *b = slotPtr(st, c->repSlot);
    hdrPut(b, &h);
    if (len > 0)
        memcpy(b + WBUS_HEADER_SIZE, payload, len);
    deliver(st, &st->eps[c->client], c->repSlot);
}

/* The server of `c` is gone: its client gets EPIPE, and the request leaves the
 * server's queue. */
static void callFail(wbus_store_t *st, call_t *c, const char *server) {
    if (c->reqSlot != NO_SLOT) {
        qRemove(st, &st->eps[c->server], c->reqSlot);
        slotFree(st, c->reqSlot);
    }
    if (c->client != NO_EP)
        replyTo(st, c, server, WBUS_STATUS_EPIPE, NULL, 0);
    c->used = false;
    poolRaise(st);
}

/* Answer every call pending at endpoint `srv` for `object`, or for all of its
 * objects when `object` is NULL. */
static void callsFail(wbus_store_t *st, int srv, const char *object) {
    const char *server = st->eps[srv].wapp->name;
    for (size_t i = 0; i < CALL_MAX; i++) {
        call_t *c = &st->calls[i];
        if (c->used && c->server == srv &&
            (object == NULL || fieldEq(c->object, object)))
            callFail(st, c, server);
    }
}

static void epRelease(wbus_store_t *st, int idx) {
    ep_t *e = &st->eps[idx];
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_OBJECTS; i++) {
        if (st->objects[i].name[0] != '\0' && st->objects[i].ep == idx) {
            callsFail(st, idx, st->objects[i].name);
            memset(&st->objects[i], 0, sizeof(st->objects[i]));
        }
    }
    for (size_t i = 0; i < CALL_MAX; i++) {
        call_t *c = &st->calls[i];
        if (!c->used || c->client != idx)
            continue;
        if (c->repSlot != NO_SLOT)
            slotFree(st, c->repSlot);
        c->repSlot = NO_SLOT;
        c->client = NO_EP;
        if (c->reqSlot != NO_SLOT) {
            qRemove(st, &st->eps[c->server], c->reqSlot);
            slotFree(st, c->reqSlot);
            c->used = false;
        }
    }
    while (e->cnt > 0)
        slotFree(st, qPop(st, e));
    e->wapp->epCnt--;
    memset(e, 0, sizeof(*e));
}

static int epOpen(wbus_store_t *st, wbus_wapp_t *w) {
    if (w->epCnt >= CONFIG_WANTED_WBUS_EP_PER_WAPP)
        return -ENOSPC;
    for (int i = 0; i < EP_MAX; i++) {
        if (st->eps[i].used)
            continue;
        memset(&st->eps[i], 0, sizeof(st->eps[i]));
        st->eps[i].used = true;
        st->eps[i].wapp = w;
        w->epCnt++;
        return i;
    }
    return -ENOSPC;
}

/* ── Operations (under the store lock) ───────────────────────────────────────
 */

static int opServe(wbus_store_t *st, int idx, const wbus_header_t *h) {
    const wbus_wapp_t *w = st->eps[idx].wapp;
    if (!allowed(w, CL_SERVE, h->object, h->member))
        return -EACCES;
    int o = objectFind(st, h->object);
    if (o >= 0)
        return st->objects[o].ep == idx ? 0 : -EBUSY;
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_OBJECTS; i++) {
        if (st->objects[i].name[0] == '\0') {
            memcpy(st->objects[i].name, h->object, WBUS_NAME_SIZE);
            st->objects[i].ep = idx;
            return 0;
        }
    }
    return -ENOSPC;
}

static int opUnserve(wbus_store_t *st, int idx, const wbus_header_t *h) {
    int o = objectFind(st, h->object);
    if (o < 0 || st->objects[o].ep != idx)
        return -EINVAL;
    callsFail(st, idx, h->object);
    memset(&st->objects[o], 0, sizeof(st->objects[o]));
    return 0;
}

static int opSubscribe(wbus_store_t *st, int idx, const wbus_header_t *h,
                       bool add) {
    ep_t *e = &st->eps[idx];
    sub_t *spare = NULL;
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_SUBS; i++) {
        sub_t *s = &e->subs[i];
        if (s->object[0] == '\0') {
            if (spare == NULL)
                spare = s;
        } else if (fieldEq(s->object, h->object) &&
                   fieldEq(s->member, h->member)) {
            if (!add)
                memset(s, 0, sizeof(*s));
            return 0;
        }
    }
    if (!add)
        return -EINVAL;
    if (!allowed(e->wapp, CL_LISTEN, h->object, h->member))
        return -EACCES;
    if (spare == NULL)
        return -ENOSPC;
    memcpy(spare->object, h->object, WBUS_NAME_SIZE);
    memcpy(spare->member, h->member, WBUS_NAME_SIZE);
    return 0;
}

/* Send the call, or return WAIT when the pool, the server's queue or the table
 * of pending calls has no room. */
static int opCall(wbus_store_t *st, int idx, const wbus_header_t *h,
                  const uint8_t *payload) {
    ep_t *e = &st->eps[idx];
    if (!allowed(e->wapp, CL_CALL, h->object, h->member))
        return -EACCES;
    int o = objectFind(st, h->object);
    if (o < 0)
        return -ENOENT;
    if (e->pending >= CONFIG_WANTED_WBUS_PENDING)
        return -EAGAIN;
    int srv = st->objects[o].ep;
    call_t *c = NULL;
    for (size_t i = 0; i < CALL_MAX && c == NULL; i++) {
        if (!st->calls[i].used)
            c = &st->calls[i];
    }
    if (c == NULL || st->freeCnt < 2 ||
        st->eps[srv].qOther >= CONFIG_WANTED_WBUS_QUEUE)
        return WAIT;

    do {
        st->nextId++;
    } while (st->nextId == 0 || callFind(st, st->nextId) != NULL);
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->engId = st->nextId;
    c->clientId = h->id;
    c->client = idx;
    c->server = srv;
    c->reqSlot = slotAlloc(st);
    c->repSlot = slotAlloc(st);
    memcpy(c->object, h->object, WBUS_NAME_SIZE);
    memcpy(c->member, h->member, WBUS_NAME_SIZE);

    wbus_header_t out = *h;
    out.id = c->engId;
    out.status = 0;
    out.dropped = 0;
    memset(out.peer, 0, WBUS_NAME_SIZE);
    memcpy(out.peer, e->wapp->name, WBUS_NAME_SIZE);
    uint8_t *b = slotPtr(st, c->reqSlot);
    hdrPut(b, &out);
    memcpy(b + WBUS_HEADER_SIZE, payload, h->len);
    e->pending++;
    deliver(st, &st->eps[srv], c->reqSlot);
    return 0;
}

static int opReply(wbus_store_t *st, int idx, const wbus_header_t *h,
                   const uint8_t *payload) {
    call_t *c = callFind(st, h->id);
    if (c == NULL || c->server != idx)
        return -EINVAL;
    if (c->client != NO_EP)
        replyTo(st, c, st->eps[idx].wapp->name, h->status, payload, h->len);
    c->used = false;
    poolRaise(st);
    return 0;
}

static bool subMatches(const ep_t *e, const wbus_header_t *h) {
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_SUBS; i++) {
        if (fieldEq(e->subs[i].object, h->object) &&
            fieldEq(e->subs[i].member, h->member))
            return true;
    }
    return false;
}

static int opEvent(wbus_store_t *st, int idx, const wbus_header_t *h,
                   const uint8_t *payload) {
    int o = objectFind(st, h->object);
    if (o < 0 || st->objects[o].ep != idx)
        return -EACCES;
    wbus_header_t out = *h;
    out.status = 0;
    out.dropped = 0;
    memcpy(out.peer, st->eps[idx].wapp->name, WBUS_NAME_SIZE);
    for (int i = 0; i < EP_MAX; i++) {
        ep_t *e = &st->eps[i];
        if (!e->used || !subMatches(e, h))
            continue;
        if (h->peer[0] != '\0' && !fieldEq(h->peer, e->wapp->name))
            continue;
        uint16_t slot =
            e->qOther < CONFIG_WANTED_WBUS_QUEUE ? slotAlloc(st) : NO_SLOT;
        if (slot == NO_SLOT) {
            if (e->dropped < UINT16_MAX)
                e->dropped++;
            e->droppedAll++;
            continue;
        }
        uint8_t *b = slotPtr(st, slot);
        hdrPut(b, &out);
        memcpy(b + WBUS_HEADER_SIZE, payload, h->len);
        deliver(st, e, slot);
    }
    return 0;
}

/* Reject a write that is not one whole, well-formed message. */
static int msgCheck(const uint8_t *buf, size_t n, wbus_header_t *h) {
    if (n < WBUS_HEADER_SIZE)
        return -EINVAL;
    hdrGet(h, buf);
    if (h->version != WBUS_VERSION || h->type < WBUS_CALL ||
        h->type > WBUS_UNSUBSCRIBE || h->reserved != 0)
        return -EINVAL;
    if (h->len > CONFIG_WANTED_WBUS_PAYLOAD_MAX)
        return -EMSGSIZE;
    if (n != (size_t)WBUS_HEADER_SIZE + h->len)
        return -EINVAL;
    int o = fieldLen(h->object);
    int m = fieldLen(h->member);
    if (o < 0 || m < 0)
        return -EINVAL;
    if (h->type == WBUS_EVENT && fieldLen(h->peer) < 0)
        return -EINVAL;
    switch (h->type) {
    case WBUS_REPLY:
        return 0;
    case WBUS_SERVE:
    case WBUS_UNSERVE:
        return o > 0 && m == 0 ? 0 : -EINVAL;
    default:
        return o > 0 && m > 0 ? 0 : -EINVAL;
    }
}

/* Sleep until the wapp's ready event fires, or for a while without one. */
static int waitReady(const wbus_wapp_t *w) {
    if (w->ready != NULL)
        return PlatformReadyWait(w->ready, -1);
    return PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, RECHECK_NS, 0);
}

static int msgWrite(wbus_wapp_t *w, int idx, const void *buf, size_t n,
                    bool nonblock) {
    wbus_store_t *st = w->store;
    wbus_header_t h;
    int rc = msgCheck(buf, n, &h);
    if (rc < 0)
        return rc;
    const uint8_t *payload = (const uint8_t *)buf + WBUS_HEADER_SIZE;

    for (;;) {
        PlatformMutexLock(st->lock);
        switch (h.type) {
        case WBUS_CALL:
            rc = opCall(st, idx, &h, payload);
            break;
        case WBUS_REPLY:
            rc = opReply(st, idx, &h, payload);
            break;
        case WBUS_EVENT:
            rc = opEvent(st, idx, &h, payload);
            break;
        case WBUS_SERVE:
            rc = opServe(st, idx, &h);
            break;
        case WBUS_UNSERVE:
            rc = opUnserve(st, idx, &h);
            break;
        case WBUS_SUBSCRIBE:
            rc = opSubscribe(st, idx, &h, true);
            break;
        default:
            rc = opSubscribe(st, idx, &h, false);
            break;
        }
        if (rc != WAIT) {
            PlatformMutexUnlock(st->lock);
            return rc < 0 ? rc : (int)n;
        }
        if (nonblock) {
            PlatformMutexUnlock(st->lock);
            return -EAGAIN;
        }
        poolWaitAdd(st, w->ready);
        PlatformMutexUnlock(st->lock);

        rc = waitReady(w);
        PlatformMutexLock(st->lock);
        poolWaitDel(st, w->ready);
        PlatformMutexUnlock(st->lock);
        if (rc == -EINTR)
            return -EINTR;
    }
}

static int msgRead(wbus_wapp_t *w, int idx, void *buf, size_t nbyte,
                   bool nonblock) {
    wbus_store_t *st = w->store;
    for (;;) {
        PlatformMutexLock(st->lock);
        ep_t *e = &st->eps[idx];
        if (e->cnt > 0) {
            uint16_t slot = e->q[e->head];
            const uint8_t *b = slotPtr(st, slot);
            size_t total = (size_t)WBUS_HEADER_SIZE + rd16(b + 2);
            if (nbyte < total) {
                PlatformMutexUnlock(st->lock);
                return -EINVAL;
            }
            memcpy(buf, b, total);
            wr16((uint8_t *)buf + 12, e->dropped);
            e->dropped = 0;
            qPop(st, e);
            if (b[1] == WBUS_REPLY)
                e->pending--;
            if (b[1] == WBUS_CALL) {
                call_t *c = callFind(st, rd32(b + 4));
                if (c != NULL)
                    c->reqSlot = NO_SLOT;
            }
            slotFree(st, slot);
            PlatformMutexUnlock(st->lock);
            return (int)total;
        }
        PlatformMutexUnlock(st->lock);
        if (nonblock)
            return -EAGAIN;
        if (waitReady(w) == -EINTR)
            return -EINTR;
    }
}

static int msgPoll(const wbus_wapp_t *w, int idx, uint32_t *avail) {
    wbus_store_t *st = w->store;
    int r = 0;
    PlatformMutexLock(st->lock);
    const ep_t *e = &st->eps[idx];
    if (e->cnt > 0) {
        r |= VFS_POLL_IN;
        if (avail != NULL)
            *avail = WBUS_HEADER_SIZE + rd16(slotPtr(st, e->q[e->head]) + 2);
    }
    if (st->freeCnt >= 2)
        r |= VFS_POLL_OUT;
    PlatformMutexUnlock(st->lock);
    return r;
}

/* ── status ──────────────────────────────────────────────────────────────────
 */

static void textAdd(char *buf, size_t *n, const char *fmt, ...) {
    if (*n >= STATUS_CAP)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(buf + *n, STATUS_CAP - *n, fmt, ap);
    va_end(ap);
    if (w < 0)
        return;
    *n += (size_t)w < STATUS_CAP - *n ? (size_t)w : STATUS_CAP - *n - 1;
}

static void textName(char *buf, size_t *n, const char *f, bool *first) {
    textAdd(buf, n, "%s%.*s", *first ? "" : ",", WBUS_NAME_SIZE, f);
    *first = false;
}

/* Snapshot the counters of the wapp's own endpoints as key=value lines. */
static size_t statusText(const wbus_wapp_t *w, char *buf) {
    wbus_store_t *st = w->store;
    size_t n = 0;
    unsigned pending = 0, queued = 0, eps = 0;
    uint32_t dropped = 0;
    bool first = true;

    PlatformMutexLock(st->lock);
    for (int i = 0; i < EP_MAX; i++) {
        const ep_t *e = &st->eps[i];
        if (!e->used || e->wapp != w)
            continue;
        eps++;
        pending += e->pending;
        queued += e->cnt;
        dropped += e->droppedAll;
    }
    textAdd(buf, &n, "endpoints=%u\nserving=", eps);
    for (size_t i = 0; i < CONFIG_WANTED_WBUS_OBJECTS; i++) {
        const object_t *o = &st->objects[i];
        if (o->name[0] != '\0' && st->eps[o->ep].wapp == w)
            textName(buf, &n, o->name, &first);
    }
    textAdd(buf, &n, "\nsubscribed=");
    first = true;
    for (int i = 0; i < EP_MAX; i++) {
        const ep_t *e = &st->eps[i];
        if (!e->used || e->wapp != w)
            continue;
        for (size_t j = 0; j < CONFIG_WANTED_WBUS_SUBS; j++) {
            if (e->subs[j].object[0] == '\0')
                continue;
            textName(buf, &n, e->subs[j].object, &first);
            textAdd(buf, &n, ".%.*s", WBUS_NAME_SIZE, e->subs[j].member);
        }
    }
    textAdd(buf, &n, "\npending=%u\nqueued=%u\ndropped=%u\npool_free=%u\n",
            pending, queued, (unsigned)dropped, (unsigned)st->freeCnt);
    PlatformMutexUnlock(st->lock);
    return n;
}

/* ── Store ───────────────────────────────────────────────────────────────────
 */

wbus_store_t *WbusStoreNew(void) {
    wbus_store_t *st = WantedMalloc(sizeof(*st));
    if (st == NULL)
        return NULL;
    memset(st, 0, sizeof(*st));
    st->pool = WantedMalloc((size_t)CONFIG_WANTED_WBUS_POOL * SLOT_SIZE);
    if (st->pool == NULL) {
        WantedFree(st);
        return NULL;
    }
    st->lock = PlatformMutexNew();
    for (uint16_t i = 0; i < CONFIG_WANTED_WBUS_POOL; i++)
        st->next[i] =
            (uint16_t)(i + 1 < CONFIG_WANTED_WBUS_POOL ? i + 1 : NO_SLOT);
    st->freeHead = 0;
    st->freeCnt = CONFIG_WANTED_WBUS_POOL;
    return st;
}

void WbusStoreFree(wbus_store_t *st) {
    if (st == NULL)
        return;
    PlatformMutexFree(st->lock);
    WantedFree(st->pool);
    WantedFree(st);
}

wbus_store_t *WbusStoreShared(void) {
    static wbus_store_t *shared = NULL;
    if (shared == NULL)
        shared = WbusStoreNew();
    return shared;
}

/* ── DevFS integration ───────────────────────────────────────────────────────
 */

static wbus_handle_t *handleGet(wbus_wapp_t *w, int fd) {
    if (fd < 0 || fd >= HANDLE_MAX)
        return NULL;
    return w->handles[fd];
}

static int _Open(vfs_driver_ctx_t dctx, const char *path, vfs_oflags_t flags) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    wbus_store_t *st = w->store;
    handle_kind_t kind;

    if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0)
        kind = H_ROOT;
    else if (strcmp(path, "msg") == 0)
        kind = H_MSG;
    else if (strcmp(path, "status") == 0)
        kind = H_STATUS;
    else
        return -ENOENT;
    if (kind == H_MSG && (flags & 3) != VFS_O_RDWR)
        return -EINVAL;
    if (kind != H_MSG && VFS_O_IS_WRITE(flags))
        return -EACCES;

    int fd = 0;
    while (fd < HANDLE_MAX && w->handles[fd] != NULL)
        fd++;
    if (fd == HANDLE_MAX)
        return -EMFILE;
    wbus_handle_t *h =
        WantedMalloc(sizeof(*h) + (kind == H_STATUS ? STATUS_CAP : 0));
    if (h == NULL)
        return -ENOMEM;
    memset(h, 0, sizeof(*h));
    h->kind = kind;
    h->ep = NO_EP;
    h->flags = flags;
    if (kind == H_MSG) {
        PlatformMutexLock(st->lock);
        h->ep = epOpen(st, w);
        PlatformMutexUnlock(st->lock);
        if (h->ep < 0) {
            int rc = h->ep;
            WantedFree(h);
            return rc;
        }
    } else if (kind == H_STATUS) {
        h->len = statusText(w, h->text);
    }
    w->handles[fd] = h;
    return fd;
}

static void handleClose(wbus_wapp_t *w, wbus_handle_t *h) {
    if (h->kind == H_MSG && h->ep >= 0) {
        PlatformMutexLock(w->store->lock);
        epRelease(w->store, h->ep);
        PlatformMutexUnlock(w->store->lock);
    }
    WantedFree(h);
}

static int _Close(vfs_driver_ctx_t dctx, int fd) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    wbus_handle_t *h = handleGet(w, fd);
    if (h == NULL)
        return -EBADF;
    w->handles[fd] = NULL;
    handleClose(w, h);
    return 0;
}

static int _Read(vfs_driver_ctx_t dctx, int fd, void *buf, size_t nbyte) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    wbus_handle_t *h = handleGet(w, fd);
    if (h == NULL || h->kind == H_ROOT)
        return -EBADF;
    if (h->kind == H_MSG)
        return msgRead(w, h->ep, buf, nbyte, (h->flags & VFS_O_NONBLOCK) != 0);
    size_t n = h->len - h->pos < nbyte ? h->len - h->pos : nbyte;
    memcpy(buf, h->text + h->pos, n);
    h->pos += n;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t dctx, int fd, const void *buf,
                  size_t nbyte) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    const wbus_handle_t *h = handleGet(w, fd);
    if (h == NULL || h->kind != H_MSG)
        return -EBADF;
    return msgWrite(w, h->ep, buf, nbyte, (h->flags & VFS_O_NONBLOCK) != 0);
}

static int _Stat(vfs_driver_ctx_t dctx, int fd, vfs_stat_t *stat) {
    const wbus_handle_t *h = handleGet((wbus_wapp_t *)dctx, fd);
    if (h == NULL)
        return -EBADF;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = VFS_FILETYPE_REGULAR_FILE;
    if (h->kind == H_ROOT)
        stat->filetype = VFS_FILETYPE_DIRECTORY;
    else if (h->kind == H_MSG)
        stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    stat->size = (uint32_t)h->len;
    return 0;
}

static int _Poll(vfs_driver_ctx_t dctx, int fd, uint32_t *avail) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    const wbus_handle_t *h = handleGet(w, fd);
    if (h == NULL)
        return -EBADF;
    if (h->kind != H_MSG)
        return VFS_POLL_IN | VFS_POLL_OUT;
    return msgPoll(w, h->ep, avail);
}

static int _SetFlags(vfs_driver_ctx_t dctx, int fd, vfs_oflags_t flags) {
    wbus_handle_t *h = handleGet((wbus_wapp_t *)dctx, fd);
    if (h == NULL)
        return -EBADF;
    h->flags = (h->flags & ~VFS_O_NONBLOCK) | (flags & VFS_O_NONBLOCK);
    return 0;
}

static void _SetReady(vfs_driver_ctx_t dctx, plat_ready_t *ready) {
    ((wbus_wapp_t *)dctx)->ready = ready;
}

/* A delivery always raises the wapp's ready event; only a poll for room needs
 * the pool's waiter list. */
static void _PollWatch(vfs_driver_ctx_t dctx, int fd, plat_ready_t *ready,
                       bool arm) {
    wbus_wapp_t *w = (wbus_wapp_t *)dctx;
    const wbus_handle_t *h = handleGet(w, fd);
    if (h == NULL || h->kind != H_MSG || ready == NULL)
        return;
    PlatformMutexLock(w->store->lock);
    if (arm)
        poolWaitAdd(w->store, ready);
    else
        poolWaitDel(w->store, ready);
    PlatformMutexUnlock(w->store->lock);
}

static int _ReadDir(vfs_driver_ctx_t dctx, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const wbus_handle_t *h = handleGet((wbus_wapp_t *)dctx, fd);
    if (h == NULL || h->kind != H_ROOT)
        return -EBADF;
    static const vfs_dir_entry_t entries[] = {
        {"msg", VFS_FILETYPE_CHARACTER_DEVICE},
        {"status", VFS_FILETYPE_REGULAR_FILE},
    };
    return VfsFlatDirReadDir(entries, 2, buf, bufLen, cookie, bufUsed);
}

static int _Destroy(vfs_driver_t *drv) {
    if (drv == NULL)
        return 0;
    wbus_wapp_t *w = (wbus_wapp_t *)drv->ctx;
    if (w != NULL) {
        for (int i = 0; i < HANDLE_MAX; i++) {
            if (w->handles[i] != NULL) {
                handleClose(w, w->handles[i]);
                w->handles[i] = NULL;
            }
        }
        WantedFree(w);
    }
    WantedFree(drv);
    return 0;
}

vfs_driver_t *WbusDriverCreate(wbus_store_t *store, const char *wapp,
                               const char *grant) {
    if (store == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    wbus_wapp_t *w = WantedMalloc(sizeof(*w));
    vfs_driver_t *drv = WantedMalloc(sizeof(*drv));
    if (w == NULL || drv == NULL) {
        WantedFree(w);
        WantedFree(drv);
        errno = ENOMEM;
        return NULL;
    }
    memset(w, 0, sizeof(*w));
    memset(drv, 0, sizeof(*drv));
    w->store = store;
    strncpy(w->name, wapp != NULL ? wapp : "", WAPP_MAX_NAME_LEN - 1);
    int rc = grantParse(w, grant);
    if (rc < 0) {
        WantedFree(w);
        WantedFree(drv);
        errno = -rc;
        return NULL;
    }

    drv->bytesId = 0x73756277; /* 'wbus' LE */
    drv->filetype = VFS_FILETYPE_DIRECTORY;
    drv->ctx = (vfs_driver_ctx_t)w;
    drv->Destroy = _Destroy;
    drv->SetReady = _SetReady;
    drv->PollWatch = _PollWatch;
    drv->raisesReady = true;
    drv->Open = _Open;
    drv->Close = _Close;
    drv->Read = _Read;
    drv->Write = _Write;
    drv->Stat = _Stat;
    drv->ReadDir = _ReadDir;
    drv->Poll = _Poll;
    drv->SetFlags = _SetFlags;
    return drv;
}

vfs_driver_t *VfsWbusInit(const wapp_t *wapp, const char *options) {
    return WbusDriverCreate(WbusStoreShared(), wapp->name, options);
}
