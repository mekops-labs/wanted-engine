/* SPDX-License-Identifier: Apache-2.0 */

#include "unity_fixture.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <c9.h>

#include <vfs.h>
#include <vfs-drivers.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * vfs_9p_local — the 9P driver against a live server over a local socket.
 *
 * A minimal 9P2000 server (the c9 server half) is forked onto a filesystem
 * socket and serves one regular file. The driver mounts it through a
 * "unix://<path>" address and the round trips — walk, open, stat, read, write
 * — are exercised for real. This is the on-box driver-server topology: an
 * out-of-engine server process reached over a socket, with no loopback port.
 * ═══════════════════════════════════════════════════════════════════════════ */

#define SOCK_PATH_FMT "/tmp/wanted-9p-test-%ld.sock"
#define SRV_MSIZE 8192u
#define SRV_FILE_NAME "hello"
#define SRV_FILE_BODY "nine-p-over-a-local-socket"

/* fid roles the test server tracks. Fid 0 is the attach root; the driver walks
 * it to a fresh fid for the file. */
#define FID_MAX 16
#define FID_FREE 0
#define FID_ROOT 1
#define FID_FILE 2

TEST_GROUP(vfs_9p_local);

static char sockPath[64];
static pid_t srvPid;
static vfs_driver_t *drv;

/* ── the forked test server ─────────────────────────────────────────────── */

struct C9aux {
    int f;
    uint8_t rBuf[SRV_MSIZE];
    uint8_t wBuf[SRV_MSIZE];
    size_t wOff;
    uint8_t fids[FID_MAX];
    char body[sizeof(SRV_FILE_BODY)];
    size_t bodyLen;
};

static const C9qid rootQid = {1, 0, C9qtdir};
static const C9qid fileQid = {2, 0, C9qtfile};

static uint8_t *srvBegin(C9ctx *ctx, uint32_t size) {
    struct C9aux *a = ctx->aux;
    uint8_t *b;

    if (a->wOff + size > sizeof(a->wBuf))
        return NULL;
    b = a->wBuf + a->wOff;
    a->wOff += size;

    return b;
}

static int srvEnd(C9ctx *ctx) {
    struct C9aux *a = ctx->aux;
    size_t n;
    ssize_t w;

    for (n = 0; n < a->wOff; n += (size_t)w) {
        if ((w = write(a->f, a->wBuf + n, a->wOff - n)) <= 0) {
            if (errno == EINTR) {
                w = 0;
                continue;
            }
            return -1;
        }
    }
    a->wOff = 0;

    return 0;
}

static uint8_t *srvRead(C9ctx *ctx, uint32_t size, int *err) {
    struct C9aux *a = ctx->aux;
    uint32_t n;
    ssize_t r;

    *err = 0;
    if (size > sizeof(a->rBuf)) {
        *err = -1;
        return NULL;
    }
    for (n = 0; n < size; n += (uint32_t)r) {
        if ((r = read(a->f, a->rBuf + n, size - n)) <= 0) {
            if (r < 0 && errno == EINTR) {
                r = 0;
                continue;
            }
            *err = r < 0 ? -1 : 0; /* clean EOF ends the loop quietly */
            return NULL;
        }
    }

    return a->rBuf;
}

/* Signature fixed by the C9ctx callback table. */
static void srvError(const char *fmt, ...) { (void)fmt; }

static void srvT(C9ctx *ctx, C9t *t) {
    struct C9aux *a = ctx->aux;
    C9stat st;
    C9qid *qids[2];
    uint32_t n;

    switch (t->type) {
    case Tversion:
        s9version(ctx);
        break;

    case Tattach:
        a->fids[t->fid % FID_MAX] = FID_ROOT;
        s9attach(ctx, t->tag, &rootQid);
        break;

    case Twalk:
        if (t->walk.wname[0] == NULL) {
            /* A zero-element walk clones the fid. */
            a->fids[t->walk.newfid % FID_MAX] = a->fids[t->fid % FID_MAX];
            qids[0] = NULL;
            s9walk(ctx, t->tag, qids);
        } else if (strcmp(t->walk.wname[0], SRV_FILE_NAME) == 0 &&
                   t->walk.wname[1] == NULL) {
            a->fids[t->walk.newfid % FID_MAX] = FID_FILE;
            qids[0] = (C9qid *)&fileQid;
            qids[1] = NULL;
            s9walk(ctx, t->tag, qids);
        } else {
            s9error(ctx, t->tag, "no such file");
        }
        break;

    case Topen:
        if (a->fids[t->fid % FID_MAX] == FID_FILE)
            s9open(ctx, t->tag, &fileQid, 0);
        else
            s9open(ctx, t->tag, &rootQid, 0);
        break;

    case Tread:
        if (a->fids[t->fid % FID_MAX] != FID_FILE) {
            s9read(ctx, t->tag, a->body, 0);
            break;
        }
        if (t->read.offset >= a->bodyLen) {
            s9read(ctx, t->tag, a->body, 0);
            break;
        }
        n = (uint32_t)(a->bodyLen - t->read.offset);
        if (n > t->read.size)
            n = t->read.size;
        s9read(ctx, t->tag, a->body + t->read.offset, n);
        break;

    case Twrite:
        n = t->write.size;
        if (t->write.offset + n > a->bodyLen)
            n = (uint32_t)(a->bodyLen - t->write.offset);
        memcpy(a->body + t->write.offset, t->write.data, n);
        s9write(ctx, t->tag, t->write.size);
        break;

    case Tstat:
        memset(&st, 0, sizeof(st));
        st.qid = fileQid;
        st.size = a->bodyLen;
        st.name = (char *)SRV_FILE_NAME;
        st.uid = (char *)"";
        st.gid = (char *)"";
        st.muid = (char *)"";
        s9stat(ctx, t->tag, &st);
        break;

    case Tclunk:
        a->fids[t->fid % FID_MAX] = FID_FREE;
        s9clunk(ctx, t->tag);
        break;

    default:
        s9error(ctx, t->tag, "not supported");
        break;
    }
}

/* Serve one connection until the peer goes away, then leave the process. */
static void srvRun(int listenFd) {
    struct C9aux aux;
    C9ctx ctx;
    int conn;

    if ((conn = accept(listenFd, NULL, NULL)) < 0)
        _exit(1);

    memset(&aux, 0, sizeof(aux));
    aux.f = conn;
    memcpy(aux.body, SRV_FILE_BODY, sizeof(SRV_FILE_BODY));
    aux.bodyLen = sizeof(SRV_FILE_BODY) - 1;

    memset(&ctx, 0, sizeof(ctx));
    ctx.read = srvRead;
    ctx.begin = srvBegin;
    ctx.end = srvEnd;
    ctx.error = srvError;
    ctx.t = srvT;
    ctx.aux = &aux;
    ctx.msize = SRV_MSIZE;

    while (s9proc(&ctx) == 0)
        ;

    _exit(0);
}

/* ── fixture ────────────────────────────────────────────────────────────── */

TEST_SETUP(vfs_9p_local) {
    struct sockaddr_un addr;
    char opt[80];
    int listenFd;

    snprintf(sockPath, sizeof(sockPath), SOCK_PATH_FMT, (long)getpid());
    unlink(sockPath);

    listenFd = socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, listenFd);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, sockPath, strlen(sockPath) + 1);
    TEST_ASSERT_EQUAL_INT(
        0, bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)));
    TEST_ASSERT_EQUAL_INT(0, listen(listenFd, 1));

    /* Bound and listening before the fork, so the driver's connect never races
     * the server's startup. */
    srvPid = fork();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, srvPid);
    if (srvPid == 0)
        srvRun(listenFd);

    close(listenFd);

    snprintf(opt, sizeof(opt), "unix://%s", sockPath);
    drv = Vfs9PInit(NULL, opt);
    TEST_ASSERT_NOT_NULL(drv);
}

TEST_TEAR_DOWN(vfs_9p_local) {
    if (drv && drv->Destroy)
        drv->Destroy(drv);
    drv = NULL;

    if (srvPid > 0) {
        kill(srvPid, SIGTERM);
        waitpid(srvPid, NULL, 0);
        srvPid = 0;
    }
    unlink(sockPath);
}

/* ── tests ──────────────────────────────────────────────────────────────── */

TEST(vfs_9p_local, Open_WalksToFile) {
    int fd = drv->Open(drv->ctx, "/" SRV_FILE_NAME, VFS_O_RDWR);
    TEST_ASSERT_GREATER_THAN_INT(0, fd);
    TEST_ASSERT_EQUAL_INT(0, drv->Close(drv->ctx, fd));
}

TEST(vfs_9p_local, Open_UnknownPath_ReturnsEio) {
    TEST_ASSERT_EQUAL_INT(-EIO, drv->Open(drv->ctx, "/nope", VFS_O_RDONLY));
}

TEST(vfs_9p_local, Read_ReturnsServerContent) {
    char buf[64] = {0};
    int fd = drv->Open(drv->ctx, "/" SRV_FILE_NAME, VFS_O_RDONLY);
    TEST_ASSERT_GREATER_THAN_INT(0, fd);

    int n = drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1);
    TEST_ASSERT_EQUAL_INT((int)strlen(SRV_FILE_BODY), n);
    TEST_ASSERT_EQUAL_STRING(SRV_FILE_BODY, buf);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_9p_local, Stat_ReportsFileSize) {
    vfs_stat_t st;
    int fd = drv->Open(drv->ctx, "/" SRV_FILE_NAME, VFS_O_RDONLY);
    TEST_ASSERT_GREATER_THAN_INT(0, fd);

    TEST_ASSERT_EQUAL_INT(0, drv->Stat(drv->ctx, fd, &st));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)strlen(SRV_FILE_BODY), st.size);

    drv->Close(drv->ctx, fd);
}

TEST(vfs_9p_local, Write_LandsOnTheServer) {
    static const char patch[] = "PATCHED";
    char buf[64] = {0};
    long pos = 0;
    int fd = drv->Open(drv->ctx, "/" SRV_FILE_NAME, VFS_O_RDWR);
    TEST_ASSERT_GREATER_THAN_INT(0, fd);

    TEST_ASSERT_EQUAL_INT((int)strlen(patch),
                          drv->Write(drv->ctx, fd, patch, strlen(patch)));

    TEST_ASSERT_EQUAL_INT(0, drv->Seek(drv->ctx, fd, 0, VFS_SEEK_SET, &pos));
    TEST_ASSERT_EQUAL_INT((int)strlen(SRV_FILE_BODY),
                          drv->Read(drv->ctx, fd, buf, sizeof(buf) - 1));
    TEST_ASSERT_EQUAL_STRING_LEN(patch, buf, strlen(patch));

    drv->Close(drv->ctx, fd);
}

TEST_GROUP_RUNNER(vfs_9p_local) {
    RUN_TEST_CASE(vfs_9p_local, Open_WalksToFile);
    RUN_TEST_CASE(vfs_9p_local, Open_UnknownPath_ReturnsEio);
    RUN_TEST_CASE(vfs_9p_local, Read_ReturnsServerContent);
    RUN_TEST_CASE(vfs_9p_local, Stat_ReportsFileSize);
    RUN_TEST_CASE(vfs_9p_local, Write_LandsOnTheServer);
}
