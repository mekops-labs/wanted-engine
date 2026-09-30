/* SPDX-License-Identifier: Apache-2.0 */

/* fbcheck: ROLE picks a writer that flushes part of /dev/fb/main, or an
 * observer that polls the damage node and checks the rectangle and pixels.
 * Each role reports its findings to its log. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define INFO "/dev/fb/main/info"
#define DATA "/dev/fb/main/data"
#define CTL "/dev/fb/main/ctl"
#define DAMAGE "/dev/fb/main/damage"
#define PEER "/net/peer"

#define BPP 2
#define FLUSH_X 4
#define FLUSH_Y 2
#define FLUSH_W 6
#define FLUSH_H 4
#define DECOY 0xEE
#define WRITER_DELAY_MS 300
#define WAIT_MS 5000
#define MIN_BLOCKED_MS 200
#define RECORD_BYTES 8

static void emit(const char *s) { write(1, s, strlen(s)); }

static long elapsedMs(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)(t1.tv_sec - t0->tv_sec) * 1000 +
           (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

/* The screen's stride, from its info line "<w> <h> <format> <stride>". */
static int stride(void) {
    char buf[64];
    int fd = open(INFO, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    unsigned w, h, s;
    char fmt[16];
    if (sscanf(buf, "%u %u %15s %u", &w, &h, fmt, &s) != 4 ||
        strcmp(fmt, "rgb565") != 0)
        return -1;
    return (int)s;
}

/* The byte the writer stores in row `y`, so a wrong row shows. */
static uint8_t patternByte(int y) { return (uint8_t)(0x40 + y); }

static int writer(void) {
    poll(NULL, 0, WRITER_DELAY_MS); /* let the observer block first */

    int stride_ = stride();
    int fd = open(DATA, O_RDWR);
    if (stride_ < 0 || fd < 0) {
        emit("fb-writer:no-screen\n");
        return 1;
    }

    /* A decoy in row 0, outside the flushed rectangle: never flushed. */
    uint8_t row[64];
    memset(row, DECOY, FLUSH_W * BPP);
    pwrite(fd, row, FLUSH_W * BPP, 0);

    for (int y = FLUSH_Y; y < FLUSH_Y + FLUSH_H; y++) {
        memset(row, patternByte(y), FLUSH_W * BPP);
        pwrite(fd, row, FLUSH_W * BPP,
               (off_t)y * stride_ + (off_t)FLUSH_X * BPP);
    }
    close(fd);

    int cfd = open(CTL, O_WRONLY);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "flush %d %d %d %d", FLUSH_X, FLUSH_Y, FLUSH_W,
             FLUSH_H);
    if (cfd < 0 || write(cfd, cmd, strlen(cmd)) < 0) {
        emit("fb-writer:flush-failed\n");
        return 1;
    }
    close(cfd);
    emit("fb-writer:flushed\n");
    return 0;
}

static int observer(void) {
    int stride_ = stride();
    int dfd = open(DATA, O_RDONLY);
    int mfd = open(DAMAGE, O_RDONLY);
    int sfd = open(PEER, O_RDWR);
    if (stride_ < 0 || dfd < 0 || mfd < 0 || sfd < 0) {
        emit("fb-observer:no-screen\n");
        return 1;
    }

    /* An observer has no way to draw or to flush. */
    int cfd = open(CTL, O_WRONLY);
    emit(cfd < 0 ? "fb-ctl:denied\n" : "fb-ctl:reachable\n");
    int wfd = open(DATA, O_RDWR);
    uint8_t px = 1;
    emit(wfd >= 0 && pwrite(wfd, &px, 1, 0) < 0 ? "fb-write:denied\n"
                                                : "fb-write:allowed\n");

    struct pollfd fds[2] = {{.fd = mfd, .events = POLLIN},
                            {.fd = sfd, .events = POLLIN}};
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int n = poll(fds, 2, WAIT_MS);
    long waited = elapsedMs(&t0);
    emit(n == 1 && (fds[0].revents & POLLIN) && !(fds[1].revents & POLLIN) &&
                 waited >= MIN_BLOCKED_MS
             ? "fb-poll:woke-on-flush\n"
             : "fb-poll:wrong\n");

    uint8_t rec[RECORD_BYTES];
    int got = (int)read(mfd, rec, sizeof(rec));
    unsigned r[4];
    for (int i = 0; i < 4 && got == RECORD_BYTES; i++)
        r[i] = (unsigned)rec[i * 2] | ((unsigned)rec[i * 2 + 1] << 8);
    emit(got == RECORD_BYTES && r[0] == FLUSH_X && r[1] == FLUSH_Y &&
                 r[2] == FLUSH_W && r[3] == FLUSH_H
             ? "fb-damage:rectangle\n"
             : "fb-damage:wrong\n");

    /* Nothing is queued: read non-blocking so the second read cannot hang. */
    int flags = fcntl(mfd, F_GETFL);
    fcntl(mfd, F_SETFL, flags | O_NONBLOCK);
    emit(read(mfd, rec, sizeof(rec)) < 0 && errno == EAGAIN
             ? "fb-damage:drained\n"
             : "fb-damage:leftover\n");

    int pattern = 1;
    uint8_t row[64];
    for (int y = FLUSH_Y; y < FLUSH_Y + FLUSH_H; y++) {
        int len = (int)pread(dfd, row, FLUSH_W * BPP,
                             (off_t)y * stride_ + (off_t)FLUSH_X * BPP);
        for (int i = 0; i < FLUSH_W * BPP && len == FLUSH_W * BPP; i++)
            pattern &= row[i] == patternByte(y);
        pattern &= len == FLUSH_W * BPP;
    }
    emit(pattern ? "fb-data:pattern\n" : "fb-data:wrong\n");

    /* The decoy was written but never flushed. */
    int hidden = pread(dfd, row, FLUSH_W * BPP, 0) == FLUSH_W * BPP;
    for (int i = 0; i < FLUSH_W * BPP && hidden; i++)
        hidden &= row[i] == 0;
    emit(hidden ? "fb-data:unflushed-hidden\n" : "fb-data:unflushed-visible\n");

    emit("fb-done\n");
    return 0;
}

int main(void) {
    const char *role = getenv("ROLE");
    return role != NULL && strcmp(role, "writer") == 0 ? writer() : observer();
}
