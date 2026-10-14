/* SPDX-License-Identifier: Apache-2.0 */

/* loracheck: drives /dev/lora/main from its launch arguments and prints what
 * the radio did. The first argument is the command (status, rx or tx); the
 * rest are key=value pairs: freq, sf, bw, sync, power, cad, secs, count, gap,
 * len and text. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CTL "/dev/lora/main/ctl"
#define RX "/dev/lora/main/rx"
#define TX "/dev/lora/main/tx"
#define STATUS "/dev/lora/main/status"

#define HEADER_BYTES 20
#define PAYLOAD_MAX 255
#define SHOW_BYTES 24
#define LINE_MAX_LEN 160
#define TENTHS 10

typedef struct opts_t {
    long freq;
    long sf;
    const char *bw;
    const char *sync;
    long power; /* -1 leaves it to the band */
    long cad;
    long secs;
    long count;
    long gap;
    long len;
    const char *text;
} opts_t;

static void emit(const char *s) { write(1, s, strlen(s)); }

static void say(const char *fmt, long a, long b, long c) {
    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line), fmt, a, b, c);
    emit(line);
}

static long nowMs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

static void parse(opts_t *o, int argc, char **argv) {
    *o = (opts_t){869525000, 11, "250", "0x12", -1, 0, 10, 1, 0, 0, "loracheck"};
    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        const char *k = argv[i];
        const char *v = eq + 1;
        if (strcmp(k, "freq") == 0) o->freq = atol(v);
        else if (strcmp(k, "sf") == 0) o->sf = atol(v);
        else if (strcmp(k, "bw") == 0) o->bw = v;
        else if (strcmp(k, "sync") == 0) o->sync = v;
        else if (strcmp(k, "power") == 0) o->power = atol(v);
        else if (strcmp(k, "cad") == 0) o->cad = atol(v);
        else if (strcmp(k, "secs") == 0) o->secs = atol(v);
        else if (strcmp(k, "count") == 0) o->count = atol(v);
        else if (strcmp(k, "gap") == 0) o->gap = atol(v);
        else if (strcmp(k, "len") == 0) o->len = atol(v);
        else if (strcmp(k, "text") == 0) o->text = v;
    }
}

/* Writes one ctl command and reports its result. */
static int ctl(int fd, const char *cmd) {
    int rc = write(fd, cmd, strlen(cmd)) < 0 ? -errno : 0;
    emit("ctl ");
    emit(cmd);
    say(" -> %ld\n", rc, 0, 0);
    return rc;
}

static int configure(int fd, const opts_t *o) {
    char cmd[LINE_MAX_LEN];
    int n = snprintf(cmd, sizeof(cmd), "set freq=%ld sf=%ld bw=%s sync=%s cad=%ld",
                     o->freq, o->sf, o->bw, o->sync, o->cad);
    if (o->power >= 0) {
        snprintf(cmd + n, sizeof(cmd) - (size_t)n, " power=%ld", o->power);
    }
    return ctl(fd, cmd);
}

static void status(void) {
    char buf[512];
    int fd = open(STATUS, O_RDONLY);
    if (fd < 0) {
        say("status open -> %ld\n", -errno, 0, 0);
        return;
    }
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n > 0) {
        buf[n] = '\0';
        emit(buf);
    }
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void show(const uint8_t *rec) {
    unsigned len = (unsigned)(rec[2] | (rec[3] << 8));
    int rssi = (int16_t)(rec[4] | (rec[5] << 8));
    int snr = (int16_t)(rec[6] | (rec[7] << 8));
    char line[LINE_MAX_LEN];
    int n = snprintf(line, sizeof(line),
                     "rx len=%u rssi=%d.%d snr=%d.%d crc=%d dropped=%u data=",
                     len, rssi / TENTHS, abs(rssi % TENTHS), snr / TENTHS,
                     abs(snr % TENTHS), rec[0] & 1, (unsigned)le32(rec + 8));
    for (unsigned i = 0; i < len && i < SHOW_BYTES; i++) {
        uint8_t c = rec[HEADER_BYTES + i];
        n += snprintf(line + n, sizeof(line) - (size_t)n, "%02x", c);
    }
    emit(line);
    emit("\n");
}

static int doRx(const opts_t *o) {
    int cfd = open(CTL, O_WRONLY);
    int rfd = open(RX, O_RDONLY);
    if (cfd < 0 || rfd < 0) {
        say("open -> %ld\n", -errno, 0, 0);
        return 1;
    }
    if (configure(cfd, o) < 0 || ctl(cfd, "start") < 0) {
        return 1;
    }
    uint8_t buf[HEADER_BYTES + PAYLOAD_MAX];
    int frames = 0;
    long end = nowMs() + o->secs * 1000;
    while (nowMs() < end) {
        struct pollfd p = {rfd, POLLIN, 0};
        int left = (int)(end - nowMs());
        if (left <= 0 || poll(&p, 1, left) <= 0) {
            break;
        }
        int n = read(rfd, buf, sizeof(buf));
        for (int off = 0; n > 0 && off + HEADER_BYTES <= n;) {
            show(buf + off);
            off += HEADER_BYTES + (buf[off + 2] | (buf[off + 3] << 8));
            frames++;
        }
    }
    say("rx done frames=%ld\n", frames, 0, 0);
    status();
    return 0;
}

static int doTx(const opts_t *o) {
    int cfd = open(CTL, O_WRONLY);
    int tfd = open(TX, O_WRONLY);
    if (cfd < 0 || tfd < 0) {
        say("open -> %ld\n", -errno, 0, 0);
        return 1;
    }
    if (configure(cfd, o) < 0) {
        return 1;
    }
    char frame[PAYLOAD_MAX];
    size_t text = strlen(o->text);
    size_t len = o->len > 0 && o->len <= PAYLOAD_MAX ? (size_t)o->len : text;
    for (size_t i = 0; i < len; i++) {
        frame[i] = o->text[i % text];
    }
    long refused = 0;
    for (long i = 0; i < o->count; i++) {
        long t0 = nowMs();
        int rc = write(tfd, frame, len) < 0 ? -errno : 0;
        refused += rc != 0;
        say("tx %ld -> %ld after %ld ms\n", i, rc, nowMs() - t0);
        if (rc == -EAGAIN) {
            break;
        }
        if (o->gap > 0) {
            poll(NULL, 0, (int)o->gap);
        }
    }
    say("tx done refused=%ld\n", refused, 0, 0);
    status();
    return 0;
}

int main(int argc, char **argv) {
    opts_t o;
    parse(&o, argc, argv);
    const char *cmd = argc > 1 ? argv[1] : "status";
    if (strcmp(cmd, "rx") == 0) {
        return doRx(&o);
    }
    if (strcmp(cmd, "tx") == 0) {
        return doTx(&o);
    }
    status();
    return 0;
}
