/* SPDX-License-Identifier: Apache-2.0 */

/* wbench: interpreter benchmark. Output format: tools/wamr-mode-bench. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPS 7
#define FIB_N 27
#define LOOP_ITERS 2000000u
#define HASH_BYTES 65536
#define COPY_BYTES 65536
#define STRIDE_BYTES 65536
#define STRIDE_STEP 61
#define SORT_N 2048
#define HOST_CALLS 5000
#define FRAME_BYTES 262144
#define CELL_W 8
#define CELL_H 16

static uint8_t frame[FRAME_BYTES];
static uint8_t bufA[HASH_BYTES];
static uint8_t bufB[COPY_BYTES];
static int sortv[SORT_N];

static int64_t nowNs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* A timed block runs until it takes at least MIN_BLOCK_NS, because some
 * targets tick the clock in 10 ms steps. The result is the time of one call. */
#define MIN_BLOCK_NS 200000000LL

static int cmpLong(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void report(const char *name, int64_t *v, int n) {
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && v[j - 1] > v[j]; j--) {
            int64_t t = v[j];
            v[j] = v[j - 1];
            v[j - 1] = t;
        }
    char line[128];
    int len = snprintf(line, sizeof(line),
                       "wbench: %s median_us=%lld.%03lld min_us=%lld.%03lld "
                       "max_us=%lld.%03lld\n",
                       name, (long long)(v[n / 2] / 1000),
                       (long long)(v[n / 2] % 1000), (long long)(v[0] / 1000),
                       (long long)(v[0] % 1000), (long long)(v[n - 1] / 1000),
                       (long long)(v[n - 1] % 1000));
    write(1, line, len);
}

static volatile uint32_t sink;
static volatile int fibN = FIB_N;
static volatile unsigned probe = 12345;

__attribute__((noinline)) static int fib(int n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static void wFib(void) { sink = (uint32_t)fib(fibN); }

static void wLoop(void) {
    uint32_t x = 1, acc = 0;
    for (uint32_t i = 0; i < LOOP_ITERS; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        acc += x & 0xff;
    }
    sink = acc;
}

static void wHash(void) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < HASH_BYTES; i++)
        h = (h ^ bufA[i]) * 16777619u;
    sink = h;
}

static void wCopyLoop(void) {
    const uint32_t *s = (const uint32_t *)bufA;
    uint32_t *d = (uint32_t *)bufB;
    uint32_t k = probe;
    for (int i = 0; i < COPY_BYTES / 4; i++)
        d[i] = s[i] ^ k;
    sink = bufB[probe % COPY_BYTES];
}

static void wCopyBulk(void) {
    memcpy(bufB, bufA, COPY_BYTES);
    sink = bufB[probe % COPY_BYTES];
}

static void wStride(void) {
    uint32_t acc = 0;
    unsigned p = 0;
    for (int i = 0; i < STRIDE_BYTES; i++) {
        acc += bufA[p];
        p = (p + STRIDE_STEP) % STRIDE_BYTES;
    }
    sink = acc;
}

static void wSort(void) {
    uint32_t x = 12345;
    for (int i = 0; i < SORT_N; i++) {
        x = x * 1103515245u + 12345u;
        sortv[i] = (int)(x >> 8);
    }
    qsort(sortv, SORT_N, sizeof(int), cmpLong);
    sink = (uint32_t)sortv[probe % SORT_N];
}

static int nullFd = -1;

static void wHostWrite(void) {
    for (int i = 0; i < HOST_CALLS; i++)
        write(nullFd, "x", 1);
}

static void wHostClock(void) {
    struct timespec t;
    for (int i = 0; i < HOST_CALLS; i++)
        clock_gettime(CLOCK_MONOTONIC, &t);
}

typedef void (*work_fn)(void);

/* The number of calls that fill one timed block, found by doubling. */
static unsigned calibrate(work_fn fn) {
    unsigned loops = 1;
    for (;;) {
        int64_t t0 = nowNs();
        for (unsigned i = 0; i < loops; i++)
            fn();
        if (nowNs() - t0 >= MIN_BLOCK_NS || loops >= (1u << 20))
            return loops;
        loops *= 2;
    }
}

static void run(const char *name, work_fn fn) {
    int64_t v[REPS];
    unsigned loops = calibrate(fn);
    for (int i = 0; i < REPS; i++) {
        int64_t t0 = nowNs();
        for (unsigned j = 0; j < loops; j++)
            fn();
        v[i] = (nowNs() - t0) / loops;
    }
    report(name, v, REPS);
}

/* --- framebuffer profiles ------------------------------------------------ */

struct screen {
    const char *name;
    unsigned w, h, stride;
    int gray4; /* else rgb565 */
    int dataFd, ctlFd;
};

static int openScreen(struct screen *s, const char *name, unsigned w,
                      unsigned h, int gray4) {
    char path[48];
    s->name = name;
    s->w = w;
    s->h = h;
    s->gray4 = gray4;
    s->stride = gray4 ? (w + 1) / 2 : w * 2;
    snprintf(path, sizeof(path), "/dev/fb/%s/data", name);
    s->dataFd = open(path, O_RDWR);
    snprintf(path, sizeof(path), "/dev/fb/%s/ctl", name);
    s->ctlFd = open(path, O_WRONLY);
    return s->dataFd < 0 || s->ctlFd < 0 ? -1 : 0;
}

static void putPx(const struct screen *s, unsigned x, unsigned y, unsigned v) {
    if (s->gray4) {
        uint8_t *p = &frame[y * s->stride + x / 2];
        *p = (x & 1) ? (uint8_t)((*p & 0xf0) | (v & 0x0f))
                     : (uint8_t)((*p & 0x0f) | ((v & 0x0f) << 4));
    } else {
        uint16_t *p = (uint16_t *)&frame[y * s->stride + x * 2];
        *p = (uint16_t)v;
    }
}

/* One 8x16 text cell: a glyph bitmap derived from the character code, drawn
 * with a foreground and background pixel value. */
static void cell(const struct screen *s, unsigned cx, unsigned cy, unsigned ch,
                 unsigned fg, unsigned bg) {
    uint32_t g = ch * 2654435761u;
    for (unsigned r = 0; r < CELL_H; r++) {
        uint32_t bits = (g >> (r & 15)) ^ (g * (r + 1));
        for (unsigned c = 0; c < CELL_W; c++)
            putPx(s, cx + c, cy + r, ((bits >> c) & 1) ? fg : bg);
    }
}

static void textRows(const struct screen *s, unsigned y0, unsigned rows,
                     unsigned seed, unsigned fg, unsigned bg) {
    for (unsigned r = 0; r < rows; r++)
        for (unsigned cx = 0; cx + CELL_W <= s->w; cx += CELL_W)
            cell(s, cx, y0 + r * CELL_H,
                 32 + ((seed + r * 7 + cx / CELL_W) % 95), fg, bg);
}

static void fillRect(const struct screen *s, unsigned x, unsigned y,
                     unsigned w, unsigned h, unsigned v) {
    for (unsigned j = 0; j < h; j++)
        for (unsigned i = 0; i < w; i++)
            putPx(s, x + i, y + j, v);
}

/* A tile: a border, a title bar and a value in large cells, like a dashboard
 * tile. */
static void tile(const struct screen *s, unsigned x, unsigned y, unsigned w,
                 unsigned h, unsigned seed) {
    fillRect(s, x, y, w, h, 15);
    fillRect(s, x, y, w, 2, 0);
    fillRect(s, x, y + h - 2, w, 2, 0);
    fillRect(s, x, y, 2, h, 0);
    fillRect(s, x + w - 2, y, 2, h, 0);
    for (unsigned cx = 0; cx + CELL_W + 8 <= w; cx += CELL_W)
        cell(s, x + 4 + cx, y + 6, 40 + ((seed + cx) % 60), 0, 15);
    for (unsigned cy = 0; cy + 3 * CELL_H <= h - 24; cy += CELL_H)
        for (unsigned cx = 0; cx + CELL_W + 8 <= w; cx += CELL_W)
            cell(s, x + 4 + cx, y + 24 + cy, 48 + ((seed + cx + cy) % 40), 0,
                 15);
}

/* Write rows [y, y+h) of columns [x, x+w) to the screen, then flush it. */
static int pushRect(const struct screen *s, unsigned x, unsigned y, unsigned w,
                    unsigned h) {
    unsigned bx = s->gray4 ? x / 2 : x * 2;
    unsigned bw = s->gray4 ? (w + 1) / 2 : w * 2;
    for (unsigned j = 0; j < h; j++)
        if (pwrite(s->dataFd, &frame[(y + j) * s->stride + bx], bw,
                   (off_t)((y + j) * s->stride + bx)) < 0)
            return -1;
    char cmd[48];
    int n = snprintf(cmd, sizeof(cmd), "flush %u %u %u %u", x, y, w, h);
    return write(s->ctlFd, cmd, n) < 0 ? -1 : 0;
}

static int pushFull(const struct screen *s) {
    if (pwrite(s->dataFd, frame, s->stride * s->h, 0) < 0)
        return -1;
    return write(s->ctlFd, "flush", 5) < 0 ? -1 : 0;
}

typedef void (*render_fn)(const struct screen *);
typedef int (*push_fn)(const struct screen *);

static const struct screen *curScreen;
static render_fn curRender;
static push_fn curPush;
static void callRender(void) { curRender(curScreen); }
static void callPush(void) { curPush(curScreen); }

static void profile(const char *name, const struct screen *s, render_fn render,
                    push_fn push) {
    int64_t r[REPS], io[REPS];
    char label[40];
    curScreen = s;
    curRender = render;
    curPush = push;
    callRender();
    if (push(s) < 0) {
        write(1, "wbench: fb push failed\n", 23);
        return;
    }
    unsigned rl = calibrate(callRender), pl = calibrate(callPush);
    for (int i = 0; i < REPS; i++) {
        int64_t t0 = nowNs();
        for (unsigned j = 0; j < rl; j++)
            callRender();
        int64_t t1 = nowNs();
        for (unsigned j = 0; j < pl; j++)
            callPush();
        r[i] = (t1 - t0) / rl;
        io[i] = (nowNs() - t1) / pl;
    }
    snprintf(label, sizeof(label), "%s.render", name);
    report(label, r, REPS);
    snprintf(label, sizeof(label), "%s.io", name);
    report(label, io, REPS);
}

static struct screen term, tender;

static void rTermFull(const struct screen *s) {
    textRows(s, 0, s->h / CELL_H, 3, 0xFFFF, 0x0000);
}
static int pTermFull(const struct screen *s) { return pushFull(s); }

static void rTermRow(const struct screen *s) {
    textRows(s, 10 * CELL_H, 1, 9, 0xFFFF, 0x0000);
}
static int pTermRow(const struct screen *s) {
    return pushRect(s, 0, 10 * CELL_H, s->w, CELL_H);
}

/* A line feed: the terminal moves the screen up one row, draws the new row at
 * the bottom, and repaints the whole screen. */
static void rTermScroll(const struct screen *s) {
    unsigned rows = s->h / CELL_H;
    memmove(frame, frame + CELL_H * s->stride, (rows - 1) * CELL_H * s->stride);
    textRows(s, (rows - 1) * CELL_H, 1, 17, 0xFFFF, 0x0000);
}
static int pTermScroll(const struct screen *s) { return pushFull(s); }

static void rTenderFull(const struct screen *s) {
    for (unsigned j = 0; j < 3; j++)
        for (unsigned i = 0; i < 2; i++)
            tile(s, 10 + i * 262, 10 + j * 318, 258, 310, i * 3 + j);
}
static int pTenderFull(const struct screen *s) { return pushFull(s); }

static void rTenderTile(const struct screen *s) {
    tile(s, 100, 200, 100, 100, 5);
}
static int pTenderTile(const struct screen *s) {
    return pushRect(s, 100, 200, 100, 100);
}

/* The six tiles refresh one after another, one flush each. */
static void rTenderGrid(const struct screen *s) { rTenderFull(s); }
static int pTenderGrid(const struct screen *s) {
    for (unsigned j = 0; j < 3; j++)
        for (unsigned i = 0; i < 2; i++)
            if (pushRect(s, 10 + i * 262, 10 + j * 318, 258, 310) < 0)
                return -1;
    return 0;
}

int main(void) {
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    char line[64];
    int n = snprintf(line, sizeof(line), "wbench: start_realtime_ns=%lld\n",
                     (long long)rt.tv_sec * 1000000000LL + rt.tv_nsec);
    write(1, line, n);

    for (int i = 0; i < HASH_BYTES; i++)
        bufA[i] = (uint8_t)(i * 31 + (i >> 8));
    nullFd = open("/dev/null", O_WRONLY);

    run("fib27", wFib);
    run("xorshift2m", wLoop);
    run("fnv64k", wHash);
    run("copy_loop64k", wCopyLoop);
    run("copy_bulk64k", wCopyBulk);
    run("stride64k", wStride);
    run("qsort2048", wSort);
    run("host_write5000", wHostWrite);
    run("host_clock5000", wHostClock);

    if (openScreen(&term, "term", 480, 222, 0) == 0) {
        profile("term_full", &term, rTermFull, pTermFull);
        profile("term_row", &term, rTermRow, pTermRow);
        profile("term_scroll", &term, rTermScroll, pTermScroll);
    } else {
        write(1, "wbench: no term screen\n", 23);
    }
    if (openScreen(&tender, "tender", 540, 960, 1) == 0) {
        profile("tender_full", &tender, rTenderFull, pTenderFull);
        profile("tender_tile", &tender, rTenderTile, pTenderTile);
        profile("tender_grid", &tender, rTenderGrid, pTenderGrid);
    } else {
        write(1, "wbench: no tender screen\n", 25);
    }
    write(1, "wbench: done\n", 13);
    /* Stay alive so the engine does not restart the run. */
    for (;;) {
        struct timespec nap = {.tv_sec = 3600};
        nanosleep(&nap, NULL);
    }
}
