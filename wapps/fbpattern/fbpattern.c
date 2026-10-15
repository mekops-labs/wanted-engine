/* SPDX-License-Identifier: Apache-2.0 */

/* fbpattern: draws a test image on a mono1 screen and flushes it. Arguments:
 * the screen name, then pattern=image|fill|clear|checker and blank=<seconds>
 * to hold the panel blank after the flush. */
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PATH_MAX_LEN 64
#define BUF_BYTES 4096
#define MS_PER_S 1000
#define BORDER 1
#define TICK_PITCH 10
#define TICK_HEIGHT 4
#define GLYPH_X 8
#define GLYPH_Y 8
#define GLYPH_STEM_W 4
#define GLYPH_H 24
#define GLYPH_BAR_W 16
#define GLYPH_MID_W 10
#define GLYPH_MID_Y 10
#define SQUARE 16
#define SQUARE_MARGIN 8
#define CELL 2

static uint8_t buf[BUF_BYTES];
static unsigned width, height, stride;

static void emit(const char *s) { write(1, s, strlen(s)); }

static void report(const char *what, int v) {
    char line[96];
    snprintf(line, sizeof(line), "fbpattern: %s %d\n", what, v);
    emit(line);
}

static void put(unsigned x, unsigned y) {
    if (x < width && y < height)
        buf[y * stride + x / 8] |= (uint8_t)(0x80 >> (x % 8));
}

static void rect(unsigned x, unsigned y, unsigned w, unsigned h) {
    for (unsigned j = 0; j < h; j++)
        for (unsigned i = 0; i < w; i++)
            put(x + i, y + j);
}

/* A border, a ruler along the top, an F that shows a mirror or a flip, and a
 * checkerboard in the bottom right corner. */
static void drawImage(void) {
    rect(0, 0, width, BORDER);
    rect(0, height - BORDER, width, BORDER);
    rect(0, 0, BORDER, height);
    rect(width - BORDER, 0, BORDER, height);
    for (unsigned x = TICK_PITCH; x < width - BORDER; x += TICK_PITCH)
        rect(x, BORDER, 1, TICK_HEIGHT);
    rect(GLYPH_X, GLYPH_Y, GLYPH_STEM_W, GLYPH_H);
    rect(GLYPH_X, GLYPH_Y, GLYPH_BAR_W, GLYPH_STEM_W);
    rect(GLYPH_X, GLYPH_Y + GLYPH_MID_Y, GLYPH_MID_W, GLYPH_STEM_W);
    unsigned ox = width - SQUARE_MARGIN - SQUARE;
    unsigned oy = height - SQUARE_MARGIN - SQUARE;
    for (unsigned j = 0; j < SQUARE; j += CELL)
        for (unsigned i = 0; i < SQUARE; i += CELL)
            if (((i + j) / CELL) % 2 == 0)
                rect(ox + i, oy + j, CELL, CELL);
}

static void drawChecker(void) {
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++)
            if ((x + y) % 2 == 0)
                put(x, y);
}

static int readInfo(const char *screen) {
    char path[PATH_MAX_LEN], text[64], fmt[16];
    snprintf(path, sizeof(path), "/dev/fb/%s/info", screen);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    int n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    text[n] = '\0';
    if (sscanf(text, "%u %u %15s %u", &width, &height, fmt, &stride) != 4 ||
        strcmp(fmt, "mono1") != 0 || stride * height > BUF_BYTES)
        return -1;
    return 0;
}

static int writeAll(const char *screen, const char *node, const void *data,
                    size_t len) {
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof(path), "/dev/fb/%s/%s", screen, node);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return -1;
    int n = write(fd, data, len);
    close(fd);
    return n == (int)len ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *screen = argc > 1 ? argv[1] : "main";
    const char *pattern = "image";
    long blank = 0;
    for (int i = 2; i < argc; i++) {
        if (strncmp(argv[i], "pattern=", 8) == 0)
            pattern = argv[i] + 8;
        else if (strncmp(argv[i], "blank=", 6) == 0)
            blank = atol(argv[i] + 6);
    }

    if (readInfo(screen) != 0) {
        emit("fbpattern: no mono1 screen\n");
        return 1;
    }
    report("width", (int)width);
    report("height", (int)height);
    report("stride", (int)stride);

    if (strcmp(pattern, "image") == 0)
        drawImage();
    else if (strcmp(pattern, "fill") == 0)
        memset(buf, 0xFF, stride * height);
    else if (strcmp(pattern, "checker") == 0)
        drawChecker();
    else if (strcmp(pattern, "clear") != 0) {
        emit("fbpattern: unknown pattern\n");
        return 1;
    }

    if (writeAll(screen, "data", buf, stride * height) != 0 ||
        writeAll(screen, "ctl", "flush\n", 6) != 0) {
        emit("fbpattern: draw failed\n");
        return 1;
    }
    emit("fbpattern: flushed\n");

    if (blank > 0) {
        writeAll(screen, "ctl", "blank on\n", 9);
        emit("fbpattern: blank on\n");
        poll(NULL, 0, (int)(blank * MS_PER_S));
        writeAll(screen, "ctl", "blank off\n", 10);
        emit("fbpattern: blank off\n");
    }
    return 0;
}
