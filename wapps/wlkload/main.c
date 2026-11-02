/* SPDX-License-Identifier: Apache-2.0 */

/* wlkload: starts the Lookout agent and flushes time-stamped frames at a fixed
 * rate. Environment and stamp layout: tools/wamr-mode-bench. */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FRAME_BYTES 262144
#define CELL_W 8
#define CELL_H 16

static uint8_t frame[FRAME_BYTES];

static unsigned W, H, STRIDE;
static int gray4;
static int dataFd, ctlFd;

static const char CHILD_CONFIG[] =
    "{\"image\":\"lookout\","
    "\"console\":{\"in\":{\"name\":\"null\"},\"out\":{\"name\":\"platform\"},"
    "\"err\":{\"name\":\"platform\"}},"
    "\"drivers\":[{\"name\":\"fb\",\"options\":\"screens=%s,observe\"}],"
    "\"sockets\":[{\"name\":\"vnc\",\"address\":\"tcp://127.0.0.1:5900\","
    "\"role\":\"listen\",\"backlog\":2,\"max_conns\":2}]}";

static int64_t nowRealUs(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

static int writeFile(const char *path, const char *text) {
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int rc = write(fd, text, strlen(text)) < 0 ? -1 : 0;
    close(fd);
    return rc;
}

static void sleepUs(int64_t us) {
    struct timespec t = {.tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000};
    nanosleep(&t, NULL);
}

static void putPx(unsigned x, unsigned y, unsigned v) {
    if (gray4) {
        uint8_t *p = &frame[y * STRIDE + x / 2];
        *p = (x & 1) ? (uint8_t)((*p & 0xf0) | (v & 0x0f))
                     : (uint8_t)((*p & 0x0f) | ((v & 0x0f) << 4));
    } else {
        *(uint16_t *)&frame[y * STRIDE + x * 2] = (uint16_t)v;
    }
}

static void cell(unsigned cx, unsigned cy, unsigned ch, unsigned fg,
                 unsigned bg) {
    uint32_t g = ch * 2654435761u;
    for (unsigned r = 0; r < CELL_H; r++) {
        uint32_t bits = (g >> (r & 15)) ^ (g * (r + 1));
        for (unsigned c = 0; c < CELL_W; c++)
            putPx(cx + c, cy + r, ((bits >> c) & 1) ? fg : bg);
    }
}

static void textRow(unsigned y0, unsigned seed) {
    unsigned fg = gray4 ? 0 : 0xFFFF, bg = gray4 ? 15 : 0x0000;
    for (unsigned cx = 0; cx + CELL_W <= W; cx += CELL_W)
        cell(cx, y0, 32 + ((seed * 7 + cx / CELL_W * 13) % 95), fg, bg);
}

/* The flush time in row 0, over the text. */
static void stamp(int64_t us) {
    uint64_t v = (uint64_t)us;
    if (gray4) {
        for (unsigned i = 0; i < 16; i++)
            putPx(i, 0, (unsigned)((v >> (60 - 4 * i)) & 0xf));
    } else {
        for (unsigned i = 0; i < 4; i++)
            putPx(i, 0, (unsigned)((v >> (48 - 16 * i)) & 0xffff));
    }
}

static void tileFrame(unsigned x, unsigned y, unsigned w, unsigned h,
                      unsigned seed) {
    for (unsigned cy = 0; cy + CELL_H <= h; cy += CELL_H)
        for (unsigned cx = 0; cx + CELL_W <= w; cx += CELL_W) {
            unsigned ch = 32 + ((seed * 5 + cx + cy) % 95);
            cell(x + cx, y + cy, ch, gray4 ? 0 : 0xFFFF, gray4 ? 15 : 0);
        }
}

static int push(unsigned x, unsigned y, unsigned w, unsigned h) {
    unsigned bx = gray4 ? x / 2 : x * 2;
    unsigned bw = gray4 ? (w + 1) / 2 : w * 2;
    for (unsigned j = 0; j < h; j++)
        if (pwrite(dataFd, &frame[(y + j) * STRIDE + bx], bw,
                   (off_t)((y + j) * STRIDE + bx)) < 0)
            return -1;
    char cmd[48];
    int n = snprintf(cmd, sizeof(cmd), "flush %u %u %u %u", x, y, w, h);
    return write(ctlFd, cmd, n) < 0 ? -1 : 0;
}

static const char *envOr(const char *k, const char *d) {
    const char *v = getenv(k);
    return v && *v ? v : d;
}

int main(void) {
    const char *screen = envOr("SCREEN", "term");
    const char *mode = envOr("MODE", "full");
    int hz = atoi(envOr("RATE_HZ", "30"));
    int seconds = atoi(envOr("SECONDS", "10"));
    int warm = atoi(envOr("WARMUP_S", "6"));

    gray4 = strcmp(screen, "tender") == 0;
    W = gray4 ? 540 : 480;
    H = gray4 ? 960 : 222;
    STRIDE = gray4 ? (W + 1) / 2 : W * 2;

    char path[48];
    snprintf(path, sizeof(path), "/dev/fb/%s/data", screen);
    dataFd = open(path, O_RDWR);
    snprintf(path, sizeof(path), "/dev/fb/%s/ctl", screen);
    ctlFd = open(path, O_WRONLY);
    if (dataFd < 0 || ctlFd < 0) {
        write(1, "wlkload: no screen\n", 19);
        return 1;
    }

    for (unsigned y = 0; y + CELL_H <= H; y += CELL_H)
        textRow(y, y / CELL_H);
    stamp(nowRealUs());
    push(0, 0, W, H);

    char cfg[640];
    snprintf(cfg, sizeof(cfg), CHILD_CONFIG, screen);
    if (writeFile("/dev/wanted/ctl", "create lookout") < 0 ||
        writeFile("/dev/wanted/wapps/lookout/config", cfg) < 0 ||
        writeFile("/dev/wanted/wapps/lookout/ctl", "start") < 0) {
        write(1, "wlkload: agent start failed\n", 28);
        return 1;
    }
    write(1, "wlkload: agent started\n", 23);
    sleepUs((int64_t)warm * 1000000);
    write(1, "wlkload: flushing\n", 18);

    int64_t period = 1000000 / hz;
    int64_t next = nowRealUs();
    int64_t frames = (int64_t)seconds * hz;
    for (int64_t f = 0; f < frames; f++) {
        int ok;
        if (strcmp(mode, "row") == 0) {
            textRow(10 * CELL_H, (unsigned)f);
            stamp(nowRealUs());
            ok = push(0, 0, W, CELL_H) == 0 &&
                 push(0, 10 * CELL_H, W, CELL_H) == 0;
        } else if (strcmp(mode, "tile") == 0) {
            tileFrame(100, 200, 100, 96, (unsigned)f);
            stamp(nowRealUs());
            ok = push(0, 0, 32, 1) == 0 && push(100, 200, 100, 96) == 0;
        } else {
            for (unsigned y = 0; y + CELL_H <= H; y += CELL_H)
                textRow(y, y / CELL_H + (unsigned)f);
            stamp(nowRealUs());
            ok = push(0, 0, W, H) == 0;
        }
        if (!ok) {
            write(1, "wlkload: push failed\n", 21);
            break;
        }
        next += period;
        int64_t wait = next - nowRealUs();
        if (wait > 0)
            sleepUs(wait);
    }
    write(1, "wlkload: done\n", 14);
    for (;;)
        sleepUs(3600000000LL);
}
