/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <vfs-drivers.h>
#include <vfs-procfs.h>
#include <vfs.h>

/* /proc/net/<name> — one flat file per socket *this wapp* holds, named after
 * its own sockets[] grant. Reads c->netfs[] directly, the same per-wapp table
 * NetFs itself resolves "/net/<name>" against, so this can only ever show a
 * wapp its own sockets — there is no cross-wapp table to reach even by
 * mistake. Unprivileged: a wapp already has fd-level access to everything
 * reported here, this just renders it as text. */

/* Upper bound on a rendered leaf body: "connected=0\n" or
 * "connected=1\nlocal=255.255.255.255:65535\n". */
#define NET_READ_MAX 40

static const vfs_named_drv_t *lookup(vfs_ctx_t c, const char *name) {
    for (uint8_t i = 0; i < c->netfs_cnt; i++) {
        if (strncmp(c->netfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return &c->netfs[i];
    }
    return NULL;
}

/* Render one socket's status into out. Returns the byte length (excluding
 * NUL); never fails — a socket with nothing to report just says so. */
static int render(const vfs_named_drv_t *e, char *out, size_t cap) {
    char addr[32];
    int r = VfsSocketLocalAddr(e->drv, addr, sizeof(addr));
    if (r < 0)
        return snprintf(out, cap, "connected=0\n");
    return snprintf(out, cap, "connected=1\nlocal=%s\n", addr);
}

static int netStat(vfs_ctx_t c, const char *sub, vfs_filetype_t *type) {
    if (sub == NULL || sub[0] == '\0') {
        *type = VFS_FILETYPE_DIRECTORY; /* /proc/net */
        return 0;
    }
    if (strchr(sub, '/') != NULL)
        return -ENOENT; /* no nesting under a socket's own entry */
    if (lookup(c, sub) == NULL)
        return -ENOENT;
    *type = VFS_FILETYPE_REGULAR_FILE;
    return 0;
}

static int netRead(vfs_ctx_t c, const char *sub, void *buf, size_t bufLen) {
    if (sub == NULL || sub[0] == '\0')
        return -EISDIR;
    const vfs_named_drv_t *e = lookup(c, sub);
    if (e == NULL)
        return -ENOENT;

    char line[NET_READ_MAX];
    int n = render(e, line, sizeof(line));
    if (n < 0)
        return n;
    if ((size_t)n > sizeof(line) - 1)
        n = (int)(sizeof(line) - 1);
    if ((size_t)n > bufLen)
        n = (int)bufLen;
    memcpy(buf, line, (size_t)n);
    return n;
}

static int netReadDir(vfs_ctx_t c, const char *sub, void *buf, size_t bufLen,
                      uint64_t *cookie, size_t *bufUsed) {
    if (sub != NULL && sub[0] != '\0')
        return -ENOTDIR; /* a socket's own entry has no children */

    vfs_dir_entry_t entries[VFS_DEVFS_MAX_ENTRIES];
    for (uint8_t i = 0; i < c->netfs_cnt; i++) {
        entries[i].name = c->netfs[i].name;
        entries[i].type = VFS_FILETYPE_REGULAR_FILE;
    }
    return VfsFlatDirReadDir(entries, c->netfs_cnt, buf, bufLen, cookie,
                             bufUsed);
}

const proc_dir_ops_t NetProcDirOps = {
    .stat = netStat,
    .read = netRead,
    .readdir = netReadDir,
};
