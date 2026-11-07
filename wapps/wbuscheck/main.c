/* SPDX-License-Identifier: Apache-2.0 */

/* wbuscheck — one image, three wbus roles chosen by the ROLE env var. `caller`
 * calls supervisor.ping and checks the reply, `denied` calls and subscribes
 * outside its grant, and `probe` runs without a wbus grant. The exit code is 0
 * when the role saw what it should. */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wbus.h"

#define REPLY_WAIT_MS 5000

static int send_msg(int fd, uint8_t type, const char *object,
                    const char *member, uint32_t id, const char *payload) {
    uint8_t buf[WBUS_HEADER_SIZE + 16];
    wbus_header_t h;
    size_t len = payload != NULL ? strlen(payload) : 0;

    memset(&h, 0, sizeof(h));
    h.version = WBUS_VERSION;
    h.type = type;
    h.len = (uint16_t)len;
    h.id = id;
    strncpy(h.object, object, WBUS_NAME_MAX);
    strncpy(h.member, member, WBUS_NAME_MAX);
    memcpy(buf, &h, sizeof(h));
    if (len > 0)
        memcpy(buf + WBUS_HEADER_SIZE, payload, len);
    return (int)write(fd, buf, WBUS_HEADER_SIZE + len);
}

static int caller(void) {
    uint8_t buf[WBUS_HEADER_SIZE + 16];
    wbus_header_t h;
    struct pollfd p;
    int fd = open(WBUS_MSG_PATH, O_RDWR);

    if (fd < 0)
        return 1;
    if (send_msg(fd, WBUS_CALL, WBUS_SUPERVISOR, "ping", 7, "ping") < 0)
        return 2;
    p.fd = fd;
    p.events = POLLIN;
    if (poll(&p, 1, REPLY_WAIT_MS) != 1)
        return 3;
    int n = (int)read(fd, buf, sizeof(buf));
    if (n != WBUS_HEADER_SIZE + 4)
        return 4;
    memcpy(&h, buf, sizeof(h));
    if (h.type != WBUS_REPLY || h.id != 7 || h.status != 0 ||
        strncmp(h.peer, WBUS_SUPERVISOR, WBUS_NAME_SIZE) != 0 ||
        memcmp(buf + WBUS_HEADER_SIZE, "pong", 4) != 0)
        return 5;
    write(STDOUT_FILENO, "wbus: pong\n", 11);
    return 0;
}

/* The grant allows supervisor.ping and nothing else. */
static int denied(void) {
    int fd = open(WBUS_MSG_PATH, O_RDWR);

    if (fd < 0)
        return 1;
    if (send_msg(fd, WBUS_CALL, WBUS_SUPERVISOR, "other", 8, NULL) >= 0 ||
        errno != EACCES)
        return 2;
    if (send_msg(fd, WBUS_SUBSCRIBE, WBUS_SUPERVISOR, "tick", 0, NULL) >= 0 ||
        errno != EACCES)
        return 3;
    if (send_msg(fd, WBUS_SERVE, "mine", "", 0, NULL) >= 0 || errno != EACCES)
        return 4;
    return 0;
}

static int probe(void) {
    if (open(WBUS_MSG_PATH, O_RDWR) >= 0 || errno != ENOENT)
        return 1;
    if (open(WBUS_STATUS_PATH, O_RDONLY) >= 0 || errno != ENOENT)
        return 2;
    return 0;
}

int main(void) {
    const char *role = getenv("ROLE");

    if (role != NULL && strcmp(role, "caller") == 0)
        return caller();
    if (role != NULL && strcmp(role, "denied") == 0)
        return denied();
    if (role != NULL && strcmp(role, "probe") == 0)
        return probe();
    return 9;
}
