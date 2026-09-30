/* SPDX-License-Identifier: Apache-2.0 */

/* inputcheck: ROLE picks an owner that polls a device's events node, an
 * injector that queues one batch, or a role blocked in a read until stopped.
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

#define INFO "/dev/input/kbd/info"
#define EVENTS "/dev/input/kbd/events"
#define INJECT "/dev/input/kbd/inject"
#define IDLE_EVENTS "/dev/input/kbd2/events"
#define PEER "/net/peer"

#define INFO_TEXT "key text rel keymap=us\n"
#define WRITER_DELAY_MS 300
#define WAIT_MS 5000
#define MIN_BLOCKED_MS 200
#define RECORD_BYTES 8
#define BATCH_RECORDS 3

/* KEY_A down, text 'a', a wheel step of -1: one physical action, one batch. */
static const uint8_t BATCH[BATCH_RECORDS * RECORD_BYTES] = {
    0x00, 0x01, 0x1E, 0x00, 0x01, 0x00, 0x00, 0x00, /* EV_KEY KEY_A 1 */
    0x00, 0xF0, 0x00, 0x00, 0x61, 0x00, 0x00, 0x00, /* EV_TEXT 'a' */
    0x01, 0x02, 0x08, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, /* EV_REL REL_WHEEL -1 */
};

static void emit(const char *s) { write(1, s, strlen(s)); }

static long elapsedMs(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (long)(t1.tv_sec - t0->tv_sec) * 1000 +
           (t1.tv_nsec - t0->tv_nsec) / 1000000;
}

static int injector(void) {
    poll(NULL, 0, WRITER_DELAY_MS); /* let the owner block first */

    /* An inject grant has no events node. */
    int efd = open(EVENTS, O_RDONLY);
    emit(efd < 0 ? "input-events:denied\n" : "input-events:reachable\n");

    int fd = open(INJECT, O_WRONLY);
    if (fd < 0 || write(fd, BATCH, sizeof(BATCH)) != (ssize_t)sizeof(BATCH)) {
        emit("input-injector:failed\n");
        return 1;
    }
    close(fd);
    emit("input-injected\n");
    return 0;
}

static int owner(void) {
    char info[64];
    int ifd = open(INFO, O_RDONLY);
    int n = ifd < 0 ? -1 : (int)read(ifd, info, sizeof(info) - 1);
    if (n > 0)
        info[n] = '\0';
    emit(n > 0 && strcmp(info, INFO_TEXT) == 0 ? "input-info:ok\n"
                                               : "input-info:wrong\n");

    /* An owner has no way to inject. */
    int jfd = open(INJECT, O_WRONLY);
    emit(jfd < 0 ? "input-inject:denied\n" : "input-inject:reachable\n");

    int efd = open(EVENTS, O_RDONLY);
    int sfd = open(PEER, O_RDWR);
    if (efd < 0 || sfd < 0) {
        emit("input-owner:no-device\n");
        return 1;
    }

    struct pollfd fds[2] = {{.fd = efd, .events = POLLIN},
                            {.fd = sfd, .events = POLLIN}};
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int ready = poll(fds, 2, WAIT_MS);
    long waited = elapsedMs(&t0);
    emit(ready == 1 && (fds[0].revents & POLLIN) && !(fds[1].revents & POLLIN) &&
                 waited >= MIN_BLOCKED_MS
             ? "input-poll:woke-on-inject\n"
             : "input-poll:wrong\n");

    uint8_t got[BATCH_RECORDS * RECORD_BYTES + RECORD_BYTES];
    int len = (int)read(efd, got, sizeof(got));
    emit(len == (int)sizeof(BATCH) && memcmp(got, BATCH, sizeof(BATCH)) == 0
             ? "input-records:ordered\n"
             : "input-records:wrong\n");

    /* Nothing is queued: read non-blocking so the second read cannot hang. */
    fcntl(efd, F_SETFL, fcntl(efd, F_GETFL) | O_NONBLOCK);
    emit(read(efd, got, RECORD_BYTES) < 0 && errno == EAGAIN
             ? "input-events:drained\n"
             : "input-events:leftover\n");

    emit("input-done\n");
    return 0;
}

static int blocked(void) {
    uint8_t rec[RECORD_BYTES];
    int fd = open(IDLE_EVENTS, O_RDONLY);
    if (fd < 0) {
        emit("input-blocked:no-device\n");
        return 1;
    }
    emit("input-blocked\n");
    read(fd, rec, sizeof(rec));
    emit("input-unblocked\n");
    return 0;
}

int main(void) {
    const char *role = getenv("ROLE");
    if (role != NULL && strcmp(role, "injector") == 0)
        return injector();
    if (role != NULL && strcmp(role, "blocked") == 0)
        return blocked();
    return owner();
}
