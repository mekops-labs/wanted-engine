/* SPDX-License-Identifier: Apache-2.0 */

/* observer — reference observability wapp, granted a `log` mount and the
 * ambient /proc but no `wanted` driver. It enumerates the fleet, tails logs,
 * and confirms every attempt to reach the control plane fails. */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define PROC_WAPPS "/proc/wapps"
#define LOG_MOUNT  "/log"
#define CONTROL    "/dev/wanted/ctl"

static void emit(const char *s) { write(1, s, strlen(s)); }

/* Read a node's value into buf (NUL-terminated); empty string on failure. */
static void read_node(const char *path, char *buf, int cap) {
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    int n = read(fd, buf, cap - 1);
    close(fd);
    if (n < 0)
        n = 0;
    buf[n] = '\0';
}

int main(void) {
    char line[128], path[192], out[256];

    /* 1) Enumerate the running fleet and read each wapp's state — read-only
     *    observability, no control authority required. */
    DIR *d = opendir(PROC_WAPPS);
    if (d != NULL) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), PROC_WAPPS "/%s/state", e->d_name);
            read_node(path, line, sizeof(line));
            snprintf(out, sizeof(out), "obs-wapp:%s=%s\n", e->d_name, line);
            emit(out);
        }
        closedir(d);
    } else {
        emit("obs-proc:unavailable\n");
    }

    /* 2) The control plane must be unreachable: this wapp was not granted the
     *    `wanted` mount, so it can observe but never command. */
    int cfd = open(CONTROL, O_WRONLY);
    if (cfd >= 0) {
        emit("obs-control:reachable\n"); /* a capability leak — must not happen */
        close(cfd);
    } else {
        emit("obs-control:denied\n");
    }

    /* 3) The granted log mount tails wapp logs without the control mount. */
    DIR *l = opendir(LOG_MOUNT);
    if (l != NULL) {
        struct dirent *e;
        while ((e = readdir(l)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(out, sizeof(out), "obs-log:%s\n", e->d_name);
            emit(out);
        }
        closedir(l);
    } else {
        emit("obs-log:unavailable\n");
    }

    emit("obs-done\n");
    return 0;
}
