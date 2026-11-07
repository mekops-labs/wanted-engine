/* SPDX-License-Identifier: Apache-2.0 */

#define _GNU_SOURCE /* memmem */

#include "unity_fixture.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-wbus.h>
#include <vfs.h>
#include <wanted-autoconf.h>
#include <wbus.h>

#define PAYLOAD CONFIG_WANTED_WBUS_PAYLOAD_MAX
#define POOL CONFIG_WANTED_WBUS_POOL
#define QUEUE CONFIG_WANTED_WBUS_QUEUE
#define PENDING CONFIG_WANTED_WBUS_PENDING
#define EP_PER_WAPP CONFIG_WANTED_WBUS_EP_PER_WAPP
#define SUBS CONFIG_WANTED_WBUS_SUBS
#define CLAUSES CONFIG_WANTED_WBUS_CLAUSES
#define OBJECTS CONFIG_WANTED_WBUS_OBJECTS
#define EPS                                                                    \
    (CONFIG_WANTED_WBUS_ENDPOINTS > 0 ? CONFIG_WANTED_WBUS_ENDPOINTS           \
                                      : CONFIG_WANTED_MAX_WAPPS + 2)
#define MAX_APPS 64

/* A wapp as the engine sees it: its driver and its ready event. */
typedef struct app_t {
    vfs_driver_t *drv;
    plat_ready_t *ready;
} app_t;

typedef struct msg_t {
    uint8_t type;
    const char *object;
    const char *member;
    const char *peer;
    uint32_t id;
    uint32_t status;
    const void *payload;
    size_t len;
} msg_t;

static wbus_store_t *store;
static app_t apps[MAX_APPS];
static int appCnt;

static void busUp(void) {
    store = WbusStoreNew();
    appCnt = 0;
    TEST_ASSERT_NOT_NULL(store);
}

static void busDown(void) {
    for (int i = 0; i < appCnt; i++) {
        apps[i].drv->Destroy(apps[i].drv);
        PlatformReadyFree(apps[i].ready);
    }
    WbusStoreFree(store);
    store = NULL;
}

static app_t *appNew(const char *name, const char *grant) {
    TEST_ASSERT_TRUE(appCnt < MAX_APPS);
    app_t *a = &apps[appCnt++];
    a->drv = WbusDriverCreate(store, name, grant);
    TEST_ASSERT_NOT_NULL_MESSAGE(a->drv, grant);
    a->ready = PlatformReadyNew();
    a->drv->SetReady(a->drv->ctx, a->ready);
    return a;
}

static int openFlags(app_t *a, vfs_oflags_t flags) {
    return a->drv->Open(a->drv->ctx, "msg", flags);
}

/* A non-blocking endpoint: a test that would hang fails instead. */
static int ep(app_t *a) { return openFlags(a, VFS_O_RDWR | VFS_O_NONBLOCK); }

static int epBlocking(app_t *a) { return openFlags(a, VFS_O_RDWR); }

static int put(app_t *a, int fd, const msg_t *m) {
    uint8_t buf[WBUS_HEADER_SIZE + PAYLOAD];
    wbus_header_t h;
    memset(&h, 0, sizeof(h));
    h.version = WBUS_VERSION;
    h.type = m->type;
    h.len = (uint16_t)m->len;
    h.id = m->id;
    h.status = m->status;
    if (m->object != NULL)
        strncpy(h.object, m->object, WBUS_NAME_SIZE);
    if (m->member != NULL)
        strncpy(h.member, m->member, WBUS_NAME_SIZE);
    if (m->peer != NULL)
        strncpy(h.peer, m->peer, WBUS_NAME_SIZE);
    memcpy(buf, &h, sizeof(h));
    if (m->len > 0)
        memcpy(buf + WBUS_HEADER_SIZE, m->payload, m->len);
    return a->drv->Write(a->drv->ctx, fd, buf, WBUS_HEADER_SIZE + m->len);
}

static int serve(app_t *a, int fd, const char *obj) {
    return put(a, fd, &(msg_t){.type = WBUS_SERVE, .object = obj});
}

static int callWith(app_t *a, int fd, const char *obj, const char *mem,
                    uint32_t id, const void *pl, size_t len) {
    return put(a, fd,
               &(msg_t){.type = WBUS_CALL,
                        .object = obj,
                        .member = mem,
                        .id = id,
                        .payload = pl,
                        .len = len});
}

static int call(app_t *a, int fd, const char *obj, const char *mem,
                uint32_t id) {
    return callWith(a, fd, obj, mem, id, NULL, 0);
}

static int reply(app_t *a, int fd, uint32_t id, uint32_t status, const void *pl,
                 size_t len) {
    return put(a, fd,
               &(msg_t){.type = WBUS_REPLY,
                        .id = id,
                        .status = status,
                        .payload = pl,
                        .len = len});
}

static int emit(app_t *a, int fd, const char *obj, const char *mem,
                const char *peer, const void *pl, size_t len) {
    return put(a, fd,
               &(msg_t){.type = WBUS_EVENT,
                        .object = obj,
                        .member = mem,
                        .peer = peer,
                        .payload = pl,
                        .len = len});
}

static int subscribe(app_t *a, int fd, const char *obj, const char *mem) {
    return put(a, fd,
               &(msg_t){.type = WBUS_SUBSCRIBE, .object = obj, .member = mem});
}

static int unsubscribe(app_t *a, int fd, const char *obj, const char *mem) {
    return put(
        a, fd,
        &(msg_t){.type = WBUS_UNSUBSCRIBE, .object = obj, .member = mem});
}

/* Read one message; the result is the read's, the header and payload are
 * copied out when it succeeds. */
static int get(app_t *a, int fd, wbus_header_t *h, uint8_t *payload) {
    uint8_t buf[WBUS_HEADER_SIZE + PAYLOAD];
    int n = a->drv->Read(a->drv->ctx, fd, buf, sizeof(buf));
    if (n >= WBUS_HEADER_SIZE) {
        memcpy(h, buf, sizeof(*h));
        if (payload != NULL)
            memcpy(payload, buf + WBUS_HEADER_SIZE,
                   (size_t)n - WBUS_HEADER_SIZE);
    }
    return n;
}

static void expectName(const char *want, const char *field) {
    char buf[WBUS_NAME_SIZE + 1];
    memcpy(buf, field, WBUS_NAME_SIZE);
    buf[WBUS_NAME_SIZE] = '\0';
    TEST_ASSERT_EQUAL_STRING(want, buf);
}

/* Read the wapp's status node and return the integer after `key=`. */
static int statusVal(app_t *a, const char *key) {
    int fd = a->drv->Open(a->drv->ctx, "status", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    char text[512];
    int n = a->drv->Read(a->drv->ctx, fd, text, sizeof(text) - 1);
    TEST_ASSERT_TRUE(n > 0);
    text[n] = '\0';
    a->drv->Close(a->drv->ctx, fd);
    char pat[32];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = text;
    while ((p = strstr(p, pat)) != NULL) {
        if (p == text || p[-1] == '\n')
            return atoi(p + strlen(pat));
        p++;
    }
    TEST_FAIL_MESSAGE(key);
    return -1;
}

static void expectPoolFull(app_t *a) {
    TEST_ASSERT_EQUAL_INT(POOL, statusVal(a, "pool_free"));
}

/* Two wapps: `svc` serves object "svc" and emits its events; `cli` calls it
 * and listens to them. */
#define SVC_GRANT "serve=svc"
#define CLI_GRANT "call=svc.*,listen=svc.*"

/***************************************/
TEST_GROUP(wbus_grant);
/***************************************/

TEST_SETUP(wbus_grant) { busUp(); }
TEST_TEAR_DOWN(wbus_grant) { busDown(); }

TEST(wbus_grant, EveryClauseKindIsAccepted) {
    app_t *a = appNew("w", "serve=a,call=b.m,call=b.*,listen=c.e,listen=c.*");
    TEST_ASSERT_NOT_NULL(a->drv);
}

TEST(wbus_grant, NoClauseFailsTheLaunch) {
    TEST_ASSERT_NULL(WbusDriverCreate(store, "w", ""));
    TEST_ASSERT_EQUAL_INT(EINVAL, errno);
    TEST_ASSERT_NULL(WbusDriverCreate(store, "w", NULL));
    TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

TEST(wbus_grant, AMalformedClauseFailsTheLaunch) {
    static const char *const bad[] = {
        "serve",     "serve=",      "bang=x",          "call=a",
        "call=a.",   "call=.m",     "call=a.m.n",      "listen=a.b*",
        "serve=*",   "call=*.m",    "serve=a b",       "serve=a,,call=b.m",
        "serve=a,",  ",serve=a",    "serve=a.b",       "serve=sixteen_chars_xx",
        "call=a.m*", "listen=a.**", "serve=a;call=b.m"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        errno = 0;
        TEST_ASSERT_NULL_MESSAGE(WbusDriverCreate(store, "w", bad[i]), bad[i]);
        TEST_ASSERT_EQUAL_INT_MESSAGE(EINVAL, errno, bad[i]);
    }
}

TEST(wbus_grant, ANameOf15CharactersIsValid) {
    app_t *a = appNew("w", "serve=fifteen_chars_x,call=A-b_9.zZ");
    TEST_ASSERT_NOT_NULL(a->drv);
}

TEST(wbus_grant, TooManyClausesFailTheLaunch) {
    char grant[CLAUSES * 8 + 16];
    grant[0] = '\0';
    for (int i = 0; i < CLAUSES; i++)
        strcat(grant, i == 0 ? "serve=a" : ",serve=a");
    app_t *a = appNew("w", grant);
    TEST_ASSERT_NOT_NULL(a->drv);
    strcat(grant, ",serve=a");
    errno = 0;
    TEST_ASSERT_NULL(WbusDriverCreate(store, "w2", grant));
    TEST_ASSERT_EQUAL_INT(ENOSPC, errno);
}

TEST(wbus_grant, OnlyTheSupervisorMayServeIt) {
    errno = 0;
    TEST_ASSERT_NULL(WbusDriverCreate(store, "other", "serve=supervisor"));
    TEST_ASSERT_EQUAL_INT(EPERM, errno);
    TEST_ASSERT_NULL(
        WbusDriverCreate(store, "other", "call=a.m,serve=supervisor"));
    app_t *s = appNew("supervisor", "serve=supervisor");
    TEST_ASSERT_NOT_NULL(s->drv);
    app_t *c = appNew("other", "call=supervisor.*,listen=supervisor.*");
    TEST_ASSERT_NOT_NULL(c->drv);
}

TEST(wbus_grant, TheTreeHoldsMsgAndStatus) {
    app_t *a = appNew("w", SVC_GRANT);
    TEST_ASSERT_EQUAL_INT(-ENOENT,
                          a->drv->Open(a->drv->ctx, "nope", VFS_O_RDWR));
    int fd = a->drv->Open(a->drv->ctx, "", VFS_O_RDONLY);
    TEST_ASSERT_TRUE(fd >= 0);
    uint8_t buf[256];
    uint64_t cookie = 0;
    size_t used = 0;
    TEST_ASSERT_EQUAL_INT(
        0, a->drv->ReadDir(a->drv->ctx, fd, buf, sizeof(buf), &cookie, &used));
    TEST_ASSERT_NOT_NULL(memmem(buf, used, "msg", 3));
    TEST_ASSERT_NOT_NULL(memmem(buf, used, "status", 6));
    a->drv->Close(a->drv->ctx, fd);
}

TEST_GROUP_RUNNER(wbus_grant) {
    RUN_TEST_CASE(wbus_grant, EveryClauseKindIsAccepted);
    RUN_TEST_CASE(wbus_grant, NoClauseFailsTheLaunch);
    RUN_TEST_CASE(wbus_grant, AMalformedClauseFailsTheLaunch);
    RUN_TEST_CASE(wbus_grant, ANameOf15CharactersIsValid);
    RUN_TEST_CASE(wbus_grant, TooManyClausesFailTheLaunch);
    RUN_TEST_CASE(wbus_grant, OnlyTheSupervisorMayServeIt);
    RUN_TEST_CASE(wbus_grant, TheTreeHoldsMsgAndStatus);
}

/***************************************/
TEST_GROUP(wbus_endpoint);
/***************************************/

TEST_SETUP(wbus_endpoint) { busUp(); }
TEST_TEAR_DOWN(wbus_endpoint) { busDown(); }

TEST(wbus_endpoint, OpenNeedsReadWrite) {
    app_t *a = appNew("w", SVC_GRANT);
    TEST_ASSERT_EQUAL_INT(-EINVAL, openFlags(a, VFS_O_RDONLY));
    TEST_ASSERT_EQUAL_INT(-EINVAL, openFlags(a, VFS_O_WRONLY));
    TEST_ASSERT_TRUE(openFlags(a, VFS_O_RDWR) >= 0);
}

TEST(wbus_endpoint, AWappHoldsAtMostItsShare) {
    app_t *a = appNew("w", SVC_GRANT);
    int fds[EP_PER_WAPP];
    for (int i = 0; i < EP_PER_WAPP; i++) {
        fds[i] = ep(a);
        TEST_ASSERT_TRUE(fds[i] >= 0);
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, ep(a));
    a->drv->Close(a->drv->ctx, fds[0]);
    TEST_ASSERT_TRUE(ep(a) >= 0);
}

TEST(wbus_endpoint, TheEngineHoldsAtMostItsEndpoints) {
    int opened = 0;
    for (int w = 0; w < EPS / EP_PER_WAPP + 2; w++) {
        char name[8];
        snprintf(name, sizeof(name), "w%d", w);
        app_t *a = appNew(name, SVC_GRANT);
        for (int i = 0; i < EP_PER_WAPP; i++) {
            int fd = ep(a);
            if (fd >= 0)
                opened++;
            else
                TEST_ASSERT_EQUAL_INT(-ENOSPC, fd);
        }
    }
    TEST_ASSERT_EQUAL_INT(EPS, opened);
}

TEST(wbus_endpoint, DestroyingTheDriverReleasesItsEndpoints) {
    app_t *a = appNew("w", SVC_GRANT);
    for (int i = 0; i < EP_PER_WAPP; i++)
        TEST_ASSERT_TRUE(ep(a) >= 0);
    a->drv->Destroy(a->drv);
    a->drv = WbusDriverCreate(store, "w", SVC_GRANT);
    a->drv->SetReady(a->drv->ctx, a->ready);
    TEST_ASSERT_TRUE(ep(a) >= 0);
}

TEST_GROUP_RUNNER(wbus_endpoint) {
    RUN_TEST_CASE(wbus_endpoint, OpenNeedsReadWrite);
    RUN_TEST_CASE(wbus_endpoint, AWappHoldsAtMostItsShare);
    RUN_TEST_CASE(wbus_endpoint, TheEngineHoldsAtMostItsEndpoints);
    RUN_TEST_CASE(wbus_endpoint, DestroyingTheDriverReleasesItsEndpoints);
}

/***************************************/
TEST_GROUP(wbus_message);
/***************************************/

static app_t *svc;
static int svcFd;

TEST_SETUP(wbus_message) {
    busUp();
    svc = appNew("svc", "serve=svc,listen=svc.*,call=svc.*");
    svcFd = ep(svc);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE, serve(svc, svcFd, "svc"));
}

TEST_TEAR_DOWN(wbus_message) { busDown(); }

TEST(wbus_message, AShortWriteIsRefused) {
    uint8_t buf[WBUS_HEADER_SIZE] = {0};
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          svc->drv->Write(svc->drv->ctx, svcFd, buf, 0));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf) - 1));
}

TEST(wbus_message, ALengthThatDisagreesWithTheHeaderIsRefused) {
    uint8_t buf[WBUS_HEADER_SIZE + 8] = {0};
    wbus_header_t h = {
        .version = WBUS_VERSION, .type = WBUS_SUBSCRIBE, .len = 4};
    strcpy(h.object, "svc");
    strcpy(h.member, "e");
    memcpy(buf, &h, sizeof(h));
    TEST_ASSERT_EQUAL_INT(-EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf,
                                                   WBUS_HEADER_SIZE + 8));
    TEST_ASSERT_EQUAL_INT(-EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf,
                                                   WBUS_HEADER_SIZE + 3));
    TEST_ASSERT_EQUAL_INT(
        WBUS_HEADER_SIZE + 4,
        svc->drv->Write(svc->drv->ctx, svcFd, buf, WBUS_HEADER_SIZE + 4));
}

TEST(wbus_message, AnUnknownVersionTypeOrReservedFieldIsRefused) {
    uint8_t buf[WBUS_HEADER_SIZE] = {0};
    wbus_header_t h = {.version = WBUS_VERSION, .type = WBUS_SUBSCRIBE};
    strcpy(h.object, "svc");
    strcpy(h.member, "e");

    wbus_header_t bad = h;
    bad.version = 2;
    memcpy(buf, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
    bad = h;
    bad.type = 0;
    memcpy(buf, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
    bad.type = WBUS_UNSUBSCRIBE + 1;
    memcpy(buf, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
    bad = h;
    bad.reserved = 1;
    memcpy(buf, &bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
}

TEST(wbus_message, APayloadPastTheLimitIsRefused) {
    uint8_t buf[WBUS_HEADER_SIZE + PAYLOAD + 1] = {0};
    wbus_header_t h = {
        .version = WBUS_VERSION, .type = WBUS_EVENT, .len = PAYLOAD + 1};
    strcpy(h.object, "svc");
    strcpy(h.member, "e");
    memcpy(buf, &h, sizeof(h));
    TEST_ASSERT_EQUAL_INT(
        -EMSGSIZE, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
    h.len = PAYLOAD;
    memcpy(buf, &h, sizeof(h));
    TEST_ASSERT_EQUAL_INT(
        (int)sizeof(buf) - 1,
        svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf) - 1));
}

TEST(wbus_message, ABadNameIsRefused) {
    TEST_ASSERT_EQUAL_INT(-EINVAL, call(svc, svcFd, "", "m", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, call(svc, svcFd, "svc", "", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, call(svc, svcFd, "sv c", "m", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, call(svc, svcFd, "svc", "m.n", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, call(svc, svcFd, "svc", "*", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          call(svc, svcFd, "sixteen_chars_xx", "m", 1));
    TEST_ASSERT_EQUAL_INT(-EINVAL, subscribe(svc, svcFd, "svc", ""));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL,
        put(svc, svcFd,
            &(msg_t){.type = WBUS_SERVE, .object = "svc", .member = "m"}));
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          emit(svc, svcFd, "svc", "e", "bad name!", NULL, 0));
}

TEST(wbus_message, AnotherByteAfterTheNulIsRefused) {
    uint8_t buf[WBUS_HEADER_SIZE] = {0};
    wbus_header_t h = {.version = WBUS_VERSION, .type = WBUS_SUBSCRIBE};
    strcpy(h.object, "svc");
    strcpy(h.member, "e");
    h.member[8] = 'x';
    memcpy(buf, &h, sizeof(h));
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, svc->drv->Write(svc->drv->ctx, svcFd, buf, sizeof(buf)));
}

TEST(wbus_message, ASmallReadBufferConsumesNothing) {
    app_t *c = appNew("cli", CLI_GRANT);
    int cfd = ep(c);
    TEST_ASSERT_TRUE(subscribe(c, cfd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(emit(svc, svcFd, "svc", "e", NULL, "abcdef", 6) > 0);
    uint8_t small[WBUS_HEADER_SIZE + 5];
    TEST_ASSERT_EQUAL_INT(-EINVAL,
                          c->drv->Read(c->drv->ctx, cfd, small, sizeof(small)));
    wbus_header_t h;
    uint8_t pl[16];
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 6, get(c, cfd, &h, pl));
    TEST_ASSERT_EQUAL_MEMORY("abcdef", pl, 6);
}

TEST(wbus_message, APayloadPassesThroughUnchanged) {
    app_t *c = appNew("cli", CLI_GRANT);
    int cfd = ep(c);
    TEST_ASSERT_TRUE(subscribe(c, cfd, "svc", "e") > 0);
    uint8_t out[PAYLOAD], in[PAYLOAD];
    for (int i = 0; i < PAYLOAD; i++)
        out[i] = (uint8_t)(i * 7 + 3);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + PAYLOAD,
                          emit(svc, svcFd, "svc", "e", NULL, out, PAYLOAD));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + PAYLOAD, get(c, cfd, &h, in));
    TEST_ASSERT_EQUAL_INT(PAYLOAD, h.len);
    TEST_ASSERT_EQUAL_MEMORY(out, in, PAYLOAD);
    expectPoolFull(svc);
}

TEST_GROUP_RUNNER(wbus_message) {
    RUN_TEST_CASE(wbus_message, AShortWriteIsRefused);
    RUN_TEST_CASE(wbus_message, ALengthThatDisagreesWithTheHeaderIsRefused);
    RUN_TEST_CASE(wbus_message, AnUnknownVersionTypeOrReservedFieldIsRefused);
    RUN_TEST_CASE(wbus_message, APayloadPastTheLimitIsRefused);
    RUN_TEST_CASE(wbus_message, ABadNameIsRefused);
    RUN_TEST_CASE(wbus_message, AnotherByteAfterTheNulIsRefused);
    RUN_TEST_CASE(wbus_message, ASmallReadBufferConsumesNothing);
    RUN_TEST_CASE(wbus_message, APayloadPassesThroughUnchanged);
}

/***************************************/
TEST_GROUP(wbus_serve);
/***************************************/

TEST_SETUP(wbus_serve) { busUp(); }
TEST_TEAR_DOWN(wbus_serve) { busDown(); }

TEST(wbus_serve, ServingNeedsAClause) {
    app_t *a = appNew("a", "call=svc.*");
    int fd = ep(a);
    TEST_ASSERT_EQUAL_INT(-EACCES, serve(a, fd, "svc"));
}

TEST(wbus_serve, OneEndpointServesAnObject) {
    app_t *a = appNew("a", "serve=svc");
    app_t *b = appNew("b", "serve=svc");
    int fa = ep(a), fb = ep(b);
    TEST_ASSERT_TRUE(serve(a, fa, "svc") > 0);
    TEST_ASSERT_EQUAL_INT(-EBUSY, serve(b, fb, "svc"));
    TEST_ASSERT_TRUE(serve(a, fa, "svc") > 0);
}

TEST(wbus_serve, UnservingFreesTheName) {
    app_t *a = appNew("a", "serve=svc");
    app_t *b = appNew("b", "serve=svc");
    int fa = ep(a), fb = ep(b);
    TEST_ASSERT_TRUE(serve(a, fa, "svc") > 0);
    TEST_ASSERT_EQUAL_INT(
        -EINVAL, put(b, fb, &(msg_t){.type = WBUS_UNSERVE, .object = "svc"}));
    TEST_ASSERT_TRUE(
        put(a, fa, &(msg_t){.type = WBUS_UNSERVE, .object = "svc"}) > 0);
    TEST_ASSERT_TRUE(serve(b, fb, "svc") > 0);
}

TEST(wbus_serve, UnservingAnswersTheCallsPending) {
    app_t *a = appNew("srv", "serve=svc");
    app_t *c = appNew("cli", CLI_GRANT);
    int fa = ep(a), fc = ep(c);
    TEST_ASSERT_TRUE(serve(a, fa, "svc") > 0);
    TEST_ASSERT_TRUE(call(c, fc, "svc", "m", 41) > 0);
    TEST_ASSERT_TRUE(
        put(a, fa, &(msg_t){.type = WBUS_UNSERVE, .object = "svc"}) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(c, fc, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_REPLY, h.type);
    TEST_ASSERT_EQUAL_UINT32(41, h.id);
    TEST_ASSERT_EQUAL_UINT32(WBUS_STATUS_EPIPE, h.status);
    expectName("srv", h.peer);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(a, fa, &h, NULL));
    expectPoolFull(c);
}

TEST(wbus_serve, TheObjectTableIsBounded) {
    int per = CLAUSES < OBJECTS ? CLAUSES : OBJECTS;
    if (OBJECTS > per * 3)
        TEST_IGNORE_MESSAGE("table too large for three wapps");
    int served = 0, refused = 0;
    for (int w = 0; w < 3; w++) {
        char name[8], grant[CLAUSES * 12 + 4] = "";
        snprintf(name, sizeof(name), "w%d", w);
        for (int i = 0; i < per; i++) {
            char one[16];
            snprintf(one, sizeof(one), "%sserve=o%d_%d", i ? "," : "", w, i);
            strcat(grant, one);
        }
        app_t *a = appNew(name, grant);
        int fd = ep(a);
        for (int i = 0; i < per; i++) {
            char obj[8];
            snprintf(obj, sizeof(obj), "o%d_%d", w, i);
            int rc = serve(a, fd, obj);
            if (rc > 0)
                served++;
            else if (rc == -ENOSPC)
                refused++;
        }
    }
    TEST_ASSERT_EQUAL_INT(OBJECTS, served);
    TEST_ASSERT_TRUE(refused > 0);
}

TEST_GROUP_RUNNER(wbus_serve) {
    RUN_TEST_CASE(wbus_serve, ServingNeedsAClause);
    RUN_TEST_CASE(wbus_serve, OneEndpointServesAnObject);
    RUN_TEST_CASE(wbus_serve, UnservingFreesTheName);
    RUN_TEST_CASE(wbus_serve, UnservingAnswersTheCallsPending);
    RUN_TEST_CASE(wbus_serve, TheObjectTableIsBounded);
}

/***************************************/
TEST_GROUP(wbus_call);
/***************************************/

static app_t *cli;
static int cliFd;

TEST_SETUP(wbus_call) {
    busUp();
    svc = appNew("svc", SVC_GRANT);
    svcFd = ep(svc);
    cli = appNew("cli", CLI_GRANT);
    cliFd = ep(cli);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
}

TEST_TEAR_DOWN(wbus_call) { busDown(); }

TEST(wbus_call, ACallAndItsReplyCarryPeerAndId) {
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 5,
                          callWith(cli, cliFd, "svc", "echo", 77, "hello", 5));
    wbus_header_t h;
    uint8_t pl[16];
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 5, get(svc, svcFd, &h, pl));
    TEST_ASSERT_EQUAL_INT(WBUS_CALL, h.type);
    expectName("svc", h.object);
    expectName("echo", h.member);
    expectName("cli", h.peer);
    TEST_ASSERT_EQUAL_MEMORY("hello", pl, 5);
    TEST_ASSERT_NOT_EQUAL(77, h.id);
    uint32_t engId = h.id;

    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 2,
                          reply(svc, svcFd, engId, 0, "ok", 2));
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 2, get(cli, cliFd, &h, pl));
    TEST_ASSERT_EQUAL_INT(WBUS_REPLY, h.type);
    TEST_ASSERT_EQUAL_UINT32(77, h.id);
    TEST_ASSERT_EQUAL_UINT32(0, h.status);
    expectName("svc", h.peer);
    expectName("svc", h.object);
    expectName("echo", h.member);
    TEST_ASSERT_EQUAL_MEMORY("ok", pl, 2);
    expectPoolFull(svc);
}

TEST(wbus_call, TheClientCannotForgeThePeer) {
    TEST_ASSERT_TRUE(put(cli, cliFd,
                         &(msg_t){.type = WBUS_CALL,
                                  .object = "svc",
                                  .member = "m",
                                  .peer = "svc",
                                  .id = 1}) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    expectName("cli", h.peer);
}

TEST(wbus_call, AReplyStatusReachesTheClient) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "nope", 5) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, WBUS_STATUS_ENOSYS, NULL, 0) > 0);
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_UINT32(WBUS_STATUS_ENOSYS, h.status);
}

TEST(wbus_call, ACallNeedsAClause) {
    app_t *x = appNew("x", "call=svc.only,listen=svc.e");
    int fd = ep(x);
    TEST_ASSERT_EQUAL_INT(-EACCES, call(x, fd, "svc", "other", 1));
    TEST_ASSERT_EQUAL_INT(-EACCES, call(x, fd, "else", "only", 1));
    TEST_ASSERT_TRUE(call(x, fd, "svc", "only", 1) > 0);
    app_t *y = appNew("y", "serve=zz");
    TEST_ASSERT_EQUAL_INT(-EACCES, call(y, ep(y), "svc", "only", 1));
}

TEST(wbus_call, ACallToNoServerFails) {
    app_t *x = appNew("x", "call=gone.*");
    TEST_ASSERT_EQUAL_INT(-ENOENT, call(x, ep(x), "gone", "m", 1));
}

TEST(wbus_call, AnEndpointHoldsAtMostItsPendingCalls) {
    for (int i = 0; i < PENDING; i++)
        TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", (uint32_t)i) > 0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, call(cli, cliFd, "svc", "m", 99));
    cli->drv->SetFlags(cli->drv->ctx, cliFd, 0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, call(cli, cliFd, "svc", "m", 99));
    cli->drv->SetFlags(cli->drv->ctx, cliFd, VFS_O_NONBLOCK);

    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, call(cli, cliFd, "svc", "m", 99));
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 99) > 0);
}

TEST(wbus_call, AnUnreadReplyKeepsItsCallPending) {
    /* The server answers every call at once; the client reads nothing. */
    wbus_header_t h;
    for (int i = 0; i < 3 * PENDING; i++) {
        int rc = call(cli, cliFd, "svc", "m", (uint32_t)i);
        if (i >= PENDING) {
            TEST_ASSERT_EQUAL_INT(-EAGAIN, rc);
            continue;
        }
        TEST_ASSERT_TRUE(rc > 0);
        TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
        TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    }
    int replies = 0;
    while (get(cli, cliFd, &h, NULL) > 0)
        replies++;
    TEST_ASSERT_EQUAL_INT(PENDING, replies);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 99) > 0);
}

TEST(wbus_call, ARepliesIdMustBeAPendingCall) {
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EINVAL, reply(svc, svcFd, 12345, 0, NULL, 0));
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, reply(svc, svcFd, h.id, 0, NULL, 0));
}

TEST(wbus_call, OnlyTheServingEndpointMayReply) {
    int other = ep(svc);
    wbus_header_t h;
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(-EINVAL, reply(svc, other, h.id, 0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(-EINVAL, reply(cli, cliFd, h.id, 0, NULL, 0));
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
}

TEST(wbus_call, AReplyTakesNoSpaceOfItsOwn) {
    wbus_header_t h;
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    TEST_ASSERT_EQUAL_INT(POOL - 2, statusVal(cli, "pool_free"));
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(POOL - 1, statusVal(cli, "pool_free"));
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    TEST_ASSERT_EQUAL_INT(POOL - 1, statusVal(cli, "pool_free"));
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    expectPoolFull(cli);
}

TEST(wbus_call, ARepliesAreNotCountedInTheQueue) {
    /* The client's queue is full of events when a reply arrives. */
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    for (int i = 0; i < QUEUE; i++)
        emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    int replies = 0, events = 0;
    while (get(cli, cliFd, &h, NULL) > 0) {
        if (h.type == WBUS_REPLY)
            replies++;
        else
            events++;
    }
    TEST_ASSERT_EQUAL_INT(1, replies);
    TEST_ASSERT_EQUAL_INT(QUEUE < POOL - 2 ? QUEUE : POOL - 2, events);
}

TEST(wbus_call, CallsReachTheServerInOrder) {
    for (uint32_t i = 0; i < PENDING; i++)
        TEST_ASSERT_TRUE(callWith(cli, cliFd, "svc", "m", 100 + i, &i, 4) > 0);
    for (uint32_t i = 0; i < PENDING; i++) {
        wbus_header_t h;
        uint8_t pl[4];
        TEST_ASSERT_TRUE(get(svc, svcFd, &h, pl) > 0);
        uint32_t n;
        memcpy(&n, pl, 4);
        TEST_ASSERT_EQUAL_UINT32(i, n);
        TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    }
    for (uint32_t i = 0; i < PENDING; i++) {
        wbus_header_t h;
        TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
        TEST_ASSERT_EQUAL_UINT32(100 + i, h.id);
    }
}

TEST(wbus_call, TheEngineAssignsADistinctIdToEachCall) {
    call(cli, cliFd, "svc", "m", 5);
    call(cli, cliFd, "svc", "m", 5);
    wbus_header_t a, b;
    TEST_ASSERT_TRUE(get(svc, svcFd, &a, NULL) > 0);
    TEST_ASSERT_TRUE(get(svc, svcFd, &b, NULL) > 0);
    TEST_ASSERT_NOT_EQUAL(a.id, b.id);
}

TEST_GROUP_RUNNER(wbus_call) {
    RUN_TEST_CASE(wbus_call, ACallAndItsReplyCarryPeerAndId);
    RUN_TEST_CASE(wbus_call, TheClientCannotForgeThePeer);
    RUN_TEST_CASE(wbus_call, AReplyStatusReachesTheClient);
    RUN_TEST_CASE(wbus_call, ACallNeedsAClause);
    RUN_TEST_CASE(wbus_call, ACallToNoServerFails);
    RUN_TEST_CASE(wbus_call, AnEndpointHoldsAtMostItsPendingCalls);
    RUN_TEST_CASE(wbus_call, AnUnreadReplyKeepsItsCallPending);
    RUN_TEST_CASE(wbus_call, ARepliesIdMustBeAPendingCall);
    RUN_TEST_CASE(wbus_call, OnlyTheServingEndpointMayReply);
    RUN_TEST_CASE(wbus_call, AReplyTakesNoSpaceOfItsOwn);
    RUN_TEST_CASE(wbus_call, ARepliesAreNotCountedInTheQueue);
    RUN_TEST_CASE(wbus_call, CallsReachTheServerInOrder);
    RUN_TEST_CASE(wbus_call, TheEngineAssignsADistinctIdToEachCall);
}

/***************************************/
TEST_GROUP(wbus_event);
/***************************************/

static app_t *cli2;
static int cli2Fd;

TEST_SETUP(wbus_event) {
    busUp();
    svc = appNew("svc", SVC_GRANT);
    svcFd = ep(svc);
    cli = appNew("cli", CLI_GRANT);
    cliFd = ep(cli);
    cli2 = appNew("cli2", CLI_GRANT);
    cli2Fd = ep(cli2);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
}

TEST_TEAR_DOWN(wbus_event) { busDown(); }

TEST(wbus_event, SubscribingNeedsAListenClause) {
    app_t *x = appNew("x", "call=svc.*,listen=svc.only");
    int fd = ep(x);
    TEST_ASSERT_EQUAL_INT(-EACCES, subscribe(x, fd, "svc", "other"));
    TEST_ASSERT_EQUAL_INT(-EACCES, subscribe(x, fd, "else", "only"));
    TEST_ASSERT_TRUE(subscribe(x, fd, "svc", "only") > 0);
}

TEST(wbus_event, EverySubscriberGetsACopy) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(subscribe(cli2, cli2Fd, "svc", "e") > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 3,
                          emit(svc, svcFd, "svc", "e", NULL, "xyz", 3));
    for (int i = 0; i < 2; i++) {
        app_t *a = i ? cli2 : cli;
        int fd = i ? cli2Fd : cliFd;
        wbus_header_t h;
        uint8_t pl[8];
        TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE + 3, get(a, fd, &h, pl));
        TEST_ASSERT_EQUAL_INT(WBUS_EVENT, h.type);
        expectName("svc", h.object);
        expectName("e", h.member);
        expectName("svc", h.peer);
        TEST_ASSERT_EQUAL_MEMORY("xyz", pl, 3);
        TEST_ASSERT_EQUAL_INT(-EAGAIN, get(a, fd, &h, NULL));
    }
    expectPoolFull(svc);
}

TEST(wbus_event, OnlySubscribersOfThatEventGetIt) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "a") > 0);
    TEST_ASSERT_TRUE(subscribe(cli2, cli2Fd, "svc", "b") > 0);
    emit(svc, svcFd, "svc", "a", NULL, NULL, 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli2, cli2Fd, &h, NULL));
}

TEST(wbus_event, OnlyTheServerMayEmit) {
    TEST_ASSERT_EQUAL_INT(-EACCES, emit(cli, cliFd, "svc", "e", NULL, NULL, 0));
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          emit(svc, svcFd, "other", "e", NULL, NULL, 0));
}

TEST(wbus_event, UnsubscribingStopsDelivery) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(unsubscribe(cli, cliFd, "svc", "e") > 0);
    emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, cliFd, &h, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, unsubscribe(cli, cliFd, "svc", "e"));
}

TEST(wbus_event, ASubscriptionNeedsNoServer) {
    app_t *x = appNew("x", "listen=late.*");
    int fd = ep(x);
    TEST_ASSERT_TRUE(subscribe(x, fd, "late", "e") > 0);
}

TEST(wbus_event, SubscribingTwiceKeepsOneSubscription) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, cliFd, &h, NULL));
}

TEST(wbus_event, TheSubscriptionTableIsBounded) {
    for (int i = 0; i < SUBS; i++) {
        char ev[8];
        snprintf(ev, sizeof(ev), "e%d", i);
        TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", ev) > 0);
    }
    TEST_ASSERT_EQUAL_INT(-ENOSPC, subscribe(cli, cliFd, "svc", "extra"));
}

TEST(wbus_event, ALostEventIsCountedOnTheNextRead) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    int sent = QUEUE + 3;
    for (int i = 0; i < sent; i++)
        TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                              emit(svc, svcFd, "svc", "e", NULL, NULL, 0));
    wbus_header_t h;
    int got = 0, dropped = -1;
    while (get(cli, cliFd, &h, NULL) > 0) {
        if (dropped < 0)
            dropped = h.dropped;
        else
            TEST_ASSERT_EQUAL_INT(0, h.dropped);
        got++;
    }
    TEST_ASSERT_TRUE(got <= QUEUE);
    TEST_ASSERT_EQUAL_INT(sent - got, dropped);
    TEST_ASSERT_TRUE(dropped >= 1);
    expectPoolFull(svc);
}

TEST(wbus_event, AnEventNeverBlocksItsEmitter) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(subscribe(cli2, cli2Fd, "svc", "e") > 0);
    svc->drv->SetFlags(svc->drv->ctx, svcFd, 0);
    for (int i = 0; i < 3 * POOL; i++)
        TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                              emit(svc, svcFd, "svc", "e", NULL, NULL, 0));
}

TEST(wbus_event, AnAddressedEventReachesOnlyThatWapp) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(subscribe(cli2, cli2Fd, "svc", "e") > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          emit(svc, svcFd, "svc", "e", "cli2", NULL, 0));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, cliFd, &h, NULL));
    TEST_ASSERT_TRUE(get(cli2, cli2Fd, &h, NULL) > 0);
    expectName("svc", h.peer);
}

TEST(wbus_event, AnAddressedEventToNoWappSucceedsAndDeliversNothing) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          emit(svc, svcFd, "svc", "e", "nobody", NULL, 0));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, cliFd, &h, NULL));
    expectPoolFull(svc);
}

TEST(wbus_event, AnAddressedEventNeedsTheWappsSubscription) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          emit(svc, svcFd, "svc", "e", "cli2", NULL, 0));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli2, cli2Fd, &h, NULL));
}

TEST(wbus_event, AnAddressedEventNeverWidensAGrant) {
    app_t *x = appNew("x", "call=svc.*");
    int fd = ep(x);
    TEST_ASSERT_EQUAL_INT(-EACCES, subscribe(x, fd, "svc", "e"));
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          emit(svc, svcFd, "svc", "e", "x", NULL, 0));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(x, fd, &h, NULL));
}

TEST(wbus_event, EventsReachASubscriberInOrder) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    for (uint8_t i = 0; i < QUEUE; i++)
        emit(svc, svcFd, "svc", "e", NULL, &i, 1);
    for (uint8_t i = 0; i < QUEUE; i++) {
        wbus_header_t h;
        uint8_t pl[1];
        TEST_ASSERT_TRUE(get(cli, cliFd, &h, pl) > 0);
        TEST_ASSERT_EQUAL_UINT8(i, pl[0]);
    }
}

TEST_GROUP_RUNNER(wbus_event) {
    RUN_TEST_CASE(wbus_event, SubscribingNeedsAListenClause);
    RUN_TEST_CASE(wbus_event, EverySubscriberGetsACopy);
    RUN_TEST_CASE(wbus_event, OnlySubscribersOfThatEventGetIt);
    RUN_TEST_CASE(wbus_event, OnlyTheServerMayEmit);
    RUN_TEST_CASE(wbus_event, UnsubscribingStopsDelivery);
    RUN_TEST_CASE(wbus_event, ASubscriptionNeedsNoServer);
    RUN_TEST_CASE(wbus_event, SubscribingTwiceKeepsOneSubscription);
    RUN_TEST_CASE(wbus_event, TheSubscriptionTableIsBounded);
    RUN_TEST_CASE(wbus_event, ALostEventIsCountedOnTheNextRead);
    RUN_TEST_CASE(wbus_event, AnEventNeverBlocksItsEmitter);
    RUN_TEST_CASE(wbus_event, AnAddressedEventReachesOnlyThatWapp);
    RUN_TEST_CASE(wbus_event,
                  AnAddressedEventToNoWappSucceedsAndDeliversNothing);
    RUN_TEST_CASE(wbus_event, AnAddressedEventNeedsTheWappsSubscription);
    RUN_TEST_CASE(wbus_event, AnAddressedEventNeverWidensAGrant);
    RUN_TEST_CASE(wbus_event, EventsReachASubscriberInOrder);
}

/***************************************/
TEST_GROUP(wbus_release);
/***************************************/

TEST_SETUP(wbus_release) {
    busUp();
    svc = appNew("svc", SVC_GRANT);
    svcFd = ep(svc);
    cli = appNew("cli", CLI_GRANT);
    cliFd = ep(cli);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
}

TEST_TEAR_DOWN(wbus_release) { busDown(); }

TEST(wbus_release, AServerClosedWithCallsPendingAnswersThemWithEpipe) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 11) > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "n", 12) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0); /* one already read */
    TEST_ASSERT_EQUAL_INT(0, svc->drv->Close(svc->drv->ctx, svcFd));

    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_REPLY, h.type);
    TEST_ASSERT_EQUAL_UINT32(11, h.id);
    TEST_ASSERT_EQUAL_UINT32(WBUS_STATUS_EPIPE, h.status);
    TEST_ASSERT_EQUAL_INT(0, h.len);
    expectName("svc", h.peer);
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_UINT32(12, h.id);
    TEST_ASSERT_EQUAL_UINT32(WBUS_STATUS_EPIPE, h.status);
    expectPoolFull(cli);
}

TEST(wbus_release, AStoppedServerAnswersTheSameWay) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 21) > 0);
    svc->drv->Destroy(svc->drv);
    svc->drv = WbusDriverCreate(store, "svc", SVC_GRANT);
    svc->drv->SetReady(svc->drv->ctx, svc->ready);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(cli, cliFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_UINT32(21, h.id);
    TEST_ASSERT_EQUAL_UINT32(WBUS_STATUS_EPIPE, h.status);
}

TEST(wbus_release, ARespawnedServerIsReachableOnlyOnceItServes) {
    svc->drv->Destroy(svc->drv);
    TEST_ASSERT_EQUAL_INT(-ENOENT, call(cli, cliFd, "svc", "m", 1));
    svc->drv = WbusDriverCreate(store, "svc", SVC_GRANT);
    svc->drv->SetReady(svc->drv->ctx, svc->ready);
    TEST_ASSERT_EQUAL_INT(-ENOENT, call(cli, cliFd, "svc", "m", 1));
    svcFd = ep(svc);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
}

TEST(wbus_release, AClientClosedBeforeTheServerReadsCancelsTheCall) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    TEST_ASSERT_EQUAL_INT(0, cli->drv->Close(cli->drv->ctx, cliFd));
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(svc, svcFd, &h, NULL));
    expectPoolFull(svc);
}

TEST(wbus_release, AServersReplyToAGoneClientSucceedsAndDeliversNothing) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_EQUAL_INT(0, cli->drv->Close(cli->drv->ctx, cliFd));
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          reply(svc, svcFd, h.id, 0, NULL, 0));
    int again = ep(cli);
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, again, &h, NULL));
    TEST_ASSERT_EQUAL_INT(-EINVAL, reply(svc, svcFd, h.id, 0, NULL, 0));
    expectPoolFull(svc);
}

TEST(wbus_release, ReleasingAnEndpointFreesItsQueuedMessages) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    for (int i = 0; i < QUEUE; i++)
        emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    TEST_ASSERT_TRUE(statusVal(svc, "pool_free") < POOL);
    TEST_ASSERT_EQUAL_INT(0, cli->drv->Close(cli->drv->ctx, cliFd));
    expectPoolFull(svc);
}

TEST(wbus_release, ReleasingAnEndpointEndsItsSubscriptions) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_EQUAL_INT(0, cli->drv->Close(cli->drv->ctx, cliFd));
    int again = ep(cli);
    emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(cli, again, &h, NULL));
}

TEST(wbus_release, AWappDestroyedWithCallsInFlightLeavesNothingBehind) {
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 2) > 0);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    cli->drv->Destroy(cli->drv);
    cli->drv = WbusDriverCreate(store, "cli", CLI_GRANT);
    cli->drv->SetReady(cli->drv->ctx, cli->ready);
    TEST_ASSERT_EQUAL_INT(WBUS_HEADER_SIZE,
                          reply(svc, svcFd, h.id, 0, NULL, 0));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(svc, svcFd, &h, NULL));
    expectPoolFull(svc);
}

TEST_GROUP_RUNNER(wbus_release) {
    RUN_TEST_CASE(wbus_release,
                  AServerClosedWithCallsPendingAnswersThemWithEpipe);
    RUN_TEST_CASE(wbus_release, AStoppedServerAnswersTheSameWay);
    RUN_TEST_CASE(wbus_release, ARespawnedServerIsReachableOnlyOnceItServes);
    RUN_TEST_CASE(wbus_release,
                  AClientClosedBeforeTheServerReadsCancelsTheCall);
    RUN_TEST_CASE(wbus_release,
                  AServersReplyToAGoneClientSucceedsAndDeliversNothing);
    RUN_TEST_CASE(wbus_release, ReleasingAnEndpointFreesItsQueuedMessages);
    RUN_TEST_CASE(wbus_release, ReleasingAnEndpointEndsItsSubscriptions);
    RUN_TEST_CASE(wbus_release,
                  AWappDestroyedWithCallsInFlightLeavesNothingBehind);
}

/***************************************/
TEST_GROUP(wbus_wait);
/***************************************/

/* svc and cli block; three fillers send without blocking to use up the room. */
#define FILLERS 3
static app_t *fill[FILLERS];
static int fillFd[FILLERS];

TEST_SETUP(wbus_wait) {
    busUp();
    if (EPS < FILLERS + 2)
        TEST_IGNORE_MESSAGE("too few endpoints for the fillers");
    svc = appNew("svc", SVC_GRANT);
    cli = appNew("cli", CLI_GRANT);
    svcFd = epBlocking(svc);
    cliFd = epBlocking(cli);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
    for (int i = 0; i < FILLERS; i++) {
        char name[8];
        snprintf(name, sizeof(name), "f%d", i);
        fill[i] = appNew(name, CLI_GRANT);
        fillFd[i] = ep(fill[i]);
    }
}

TEST_TEAR_DOWN(wbus_wait) { busDown(); }

typedef struct io_t {
    app_t *app;
    int fd;
    int rc;
    wbus_header_t h;
} io_t;

static void *readThread(void *arg) {
    io_t *io = arg;
    io->rc = get(io->app, io->fd, &io->h, NULL);
    return NULL;
}

static void *callThread(void *arg) {
    io_t *io = arg;
    io->rc = call(io->app, io->fd, "svc", "m", 500);
    return NULL;
}

/* Send calls from every filler until one is refused; the count sent. The
 * server's queue or the pool is full unless the pending caps stop them first.
 */
static int fillRoom(int *lastRc) {
    int sent = 0;
    for (int i = 0; i < FILLERS; i++) {
        for (;;) {
            *lastRc = call(fill[i], fillFd[i], "svc", "m", (uint32_t)sent);
            if (*lastRc <= 0)
                break;
            sent++;
        }
    }
    return sent;
}

static int roomLimit(void) { return QUEUE < POOL / 2 ? QUEUE : POOL / 2; }

/* The server answers every call it was sent, and each filler reads its replies:
 * nothing stays reserved. */
static void drainRoom(void) {
    wbus_header_t h;
    svc->drv->SetFlags(svc->drv->ctx, svcFd, VFS_O_NONBLOCK);
    while (get(svc, svcFd, &h, NULL) > 0) {
        TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    }
}

TEST(wbus_wait, ABlockedReadWakesOnADelivery) {
    io_t io = {.app = svc, .fd = svcFd, .rc = 1};
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, readThread, &io));
    usleep(30000);
    TEST_ASSERT_EQUAL_INT(1, io.rc);
    TEST_ASSERT_TRUE(call(fill[0], fillFd[0], "svc", "m", 3) > 0);
    pthread_join(t, NULL);
    TEST_ASSERT_TRUE(io.rc > 0);
    TEST_ASSERT_EQUAL_INT(WBUS_CALL, io.h.type);
}

TEST(wbus_wait, AStopEndsABlockedRead) {
    io_t io = {.app = cli, .fd = cliFd, .rc = 1};
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, readThread, &io));
    usleep(30000);
    PlatformReadyStop(cli->ready);
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_INT(-EINTR, io.rc);
}

TEST(wbus_wait, ANonBlockingReadReturnsAtOnce) {
    wbus_header_t h;
    TEST_ASSERT_EQUAL_INT(-EAGAIN, get(fill[0], fillFd[0], &h, NULL));
}

TEST(wbus_wait, ANonBlockingCallFailsWhenTheServerHasNoRoom) {
    if (FILLERS * PENDING < roomLimit())
        TEST_IGNORE_MESSAGE("the pending caps stop the fill first");
    int last = 0;
    TEST_ASSERT_EQUAL_INT(roomLimit(), fillRoom(&last));
    TEST_ASSERT_EQUAL_INT(-EAGAIN, last);
    drainRoom();
    for (int i = 0; i < FILLERS; i++) {
        wbus_header_t h;
        while (get(fill[i], fillFd[i], &h, NULL) > 0)
            ;
    }
    expectPoolFull(svc);
    TEST_ASSERT_TRUE(call(fill[0], fillFd[0], "svc", "m", 1) > 0);
}

TEST(wbus_wait, ABlockedCallWaitsForRoom) {
    if (FILLERS * PENDING < roomLimit())
        TEST_IGNORE_MESSAGE("the pending caps stop the fill first");
    int last = 0;
    fillRoom(&last);
    io_t io = {.app = cli, .fd = cliFd, .rc = 1};
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, callThread, &io));
    usleep(30000);
    TEST_ASSERT_EQUAL_INT(1, io.rc);
    wbus_header_t h;
    TEST_ASSERT_TRUE(get(svc, svcFd, &h, NULL) > 0);
    TEST_ASSERT_TRUE(reply(svc, svcFd, h.id, 0, NULL, 0) > 0);
    TEST_ASSERT_EQUAL_INT(1, io.rc);
    /* the replied filler reads its reply, which frees the reserved slot */
    for (int i = 0; i < FILLERS; i++)
        get(fill[i], fillFd[i], &h, NULL);
    pthread_join(t, NULL);
    TEST_ASSERT_TRUE(io.rc > 0);
}

TEST(wbus_wait, AStopEndsABlockedCall) {
    if (FILLERS * PENDING < roomLimit())
        TEST_IGNORE_MESSAGE("the pending caps stop the fill first");
    int last = 0;
    fillRoom(&last);
    io_t io = {.app = cli, .fd = cliFd, .rc = 1};
    pthread_t t;
    TEST_ASSERT_EQUAL_INT(0, pthread_create(&t, NULL, callThread, &io));
    usleep(30000);
    PlatformReadyStop(cli->ready);
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_INT(-EINTR, io.rc);
}

TEST(wbus_wait, PollReportsAQueuedMessageAndRoomForACall) {
    uint32_t avail = 0;
    TEST_ASSERT_EQUAL_INT(VFS_POLL_OUT,
                          svc->drv->Poll(svc->drv->ctx, svcFd, &avail));
    TEST_ASSERT_TRUE(call(fill[0], fillFd[0], "svc", "m", 1) > 0);
    TEST_ASSERT_EQUAL_INT(VFS_POLL_IN | VFS_POLL_OUT,
                          svc->drv->Poll(svc->drv->ctx, svcFd, &avail));
    TEST_ASSERT_EQUAL_UINT32(WBUS_HEADER_SIZE, avail);
}

TEST(wbus_wait, PollStopsReportingRoomWhenThePoolIsFull) {
    if (FILLERS * PENDING < roomLimit())
        TEST_IGNORE_MESSAGE("the pending caps stop the fill first");
    int last = 0;
    fillRoom(&last);
    uint32_t avail = 0;
    int r = fill[0]->drv->Poll(fill[0]->drv->ctx, fillFd[0], &avail);
    TEST_ASSERT_EQUAL_INT(0, r & VFS_POLL_OUT);
}

TEST(wbus_wait, PollWatchArmsAndDisarmsTheWaiter) {
    TEST_ASSERT_TRUE(svc->drv->raisesReady);
    svc->drv->PollWatch(svc->drv->ctx, svcFd, svc->ready, true);
    svc->drv->PollWatch(svc->drv->ctx, svcFd, svc->ready, false);
    svc->drv->PollWatch(svc->drv->ctx, svcFd, NULL, true);
}

TEST_GROUP_RUNNER(wbus_wait) {
    RUN_TEST_CASE(wbus_wait, ABlockedReadWakesOnADelivery);
    RUN_TEST_CASE(wbus_wait, AStopEndsABlockedRead);
    RUN_TEST_CASE(wbus_wait, ANonBlockingReadReturnsAtOnce);
    RUN_TEST_CASE(wbus_wait, ANonBlockingCallFailsWhenTheServerHasNoRoom);
    RUN_TEST_CASE(wbus_wait, ABlockedCallWaitsForRoom);
    RUN_TEST_CASE(wbus_wait, AStopEndsABlockedCall);
    RUN_TEST_CASE(wbus_wait, PollReportsAQueuedMessageAndRoomForACall);
    RUN_TEST_CASE(wbus_wait, PollStopsReportingRoomWhenThePoolIsFull);
    RUN_TEST_CASE(wbus_wait, PollWatchArmsAndDisarmsTheWaiter);
}

/***************************************/
TEST_GROUP(wbus_status);
/***************************************/

TEST_SETUP(wbus_status) {
    busUp();
    svc = appNew("svc", SVC_GRANT);
    cli = appNew("cli", CLI_GRANT);
    svcFd = ep(svc);
    cliFd = ep(cli);
    TEST_ASSERT_TRUE(serve(svc, svcFd, "svc") > 0);
}

TEST_TEAR_DOWN(wbus_status) { busDown(); }

TEST(wbus_status, ItListsTheKeysOfTheWappsOwnEndpoints) {
    int fd = svc->drv->Open(svc->drv->ctx, "status", VFS_O_RDONLY);
    char text[512];
    int n = svc->drv->Read(svc->drv->ctx, fd, text, sizeof(text) - 1);
    TEST_ASSERT_TRUE(n > 0);
    text[n] = '\0';
    TEST_ASSERT_NOT_NULL(strstr(text, "endpoints=1\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "serving=svc\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "subscribed=\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "pending=0\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "queued=0\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "dropped=0\n"));
    TEST_ASSERT_NOT_NULL(strstr(text, "pool_free="));
    TEST_ASSERT_EQUAL_INT(
        0, svc->drv->Read(svc->drv->ctx, fd, text, sizeof(text)));
    svc->drv->Close(svc->drv->ctx, fd);
}

TEST(wbus_status, ItCountsOnlyTheReadersOwnWork) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    TEST_ASSERT_TRUE(call(cli, cliFd, "svc", "m", 1) > 0);
    emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    TEST_ASSERT_EQUAL_INT(1, statusVal(cli, "pending"));
    TEST_ASSERT_EQUAL_INT(1, statusVal(cli, "queued"));
    TEST_ASSERT_EQUAL_INT(0, statusVal(svc, "pending"));
    TEST_ASSERT_EQUAL_INT(1, statusVal(svc, "queued"));
    TEST_ASSERT_EQUAL_INT(1, statusVal(cli, "endpoints"));
}

TEST(wbus_status, ItReportsTheEventsAnEndpointLost) {
    TEST_ASSERT_TRUE(subscribe(cli, cliFd, "svc", "e") > 0);
    for (int i = 0; i < QUEUE + 2; i++)
        emit(svc, svcFd, "svc", "e", NULL, NULL, 0);
    TEST_ASSERT_TRUE(statusVal(cli, "dropped") >= 2);
}

TEST(wbus_status, ItIsReadOnly) {
    TEST_ASSERT_EQUAL_INT(-EACCES,
                          svc->drv->Open(svc->drv->ctx, "status", VFS_O_RDWR));
    TEST_ASSERT_EQUAL_INT(
        -EACCES, svc->drv->Open(svc->drv->ctx, "status", VFS_O_WRONLY));
    int fd = svc->drv->Open(svc->drv->ctx, "status", VFS_O_RDONLY);
    TEST_ASSERT_EQUAL_INT(-EBADF, svc->drv->Write(svc->drv->ctx, fd, "x", 1));
}

TEST_GROUP_RUNNER(wbus_status) {
    RUN_TEST_CASE(wbus_status, ItListsTheKeysOfTheWappsOwnEndpoints);
    RUN_TEST_CASE(wbus_status, ItCountsOnlyTheReadersOwnWork);
    RUN_TEST_CASE(wbus_status, ItReportsTheEventsAnEndpointLost);
    RUN_TEST_CASE(wbus_status, ItIsReadOnly);
}
