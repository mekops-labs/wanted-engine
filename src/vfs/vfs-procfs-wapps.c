/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <platform.h>
#include <vfs-procfs.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>

/* /proc/wapps/<name>/<leaf> — the read-only observability view of every running
 * wapp. A directory entry of the flat ProcFS (vfs-procfs.c) whose children are
 * resolved here from PlatformWappGetState, the same source the control plane
 * renders state from. This surface carries no control verbs: it is reachable
 * without the /dev/wanted mount, so an observer can watch the fleet without the
 * authority to command it. */

/* Longest leaf token we emit; bounds the per-segment parse buffer. */
#define WAPP_LEAF_MAX 16

/* Upper bound on a rendered leaf body. The single-value leaves fit easily; the
 * multi-line `memory` body is the widest. */
#define WAPP_READ_MAX 128

/* Leaves under /proc/wapps/<name>/. */
static const char *const LEAVES[] = {"state", "image",  "version",
                                     "id",    "memory", "exit_code"};
#define N_LEAVES (sizeof(LEAVES) / sizeof(LEAVES[0]))

/* Split a sub-path into its wapp name and leaf. Accepted forms (no leading
 * slash): "" → both empty (the wapps directory); "<name>" or "<name>/" → name
 * set, leaf empty (a wapp directory); "<name>/<leaf>" → both set. Returns
 * -ENAMETOOLONG if a segment overflows, -ENOENT for a path deeper than a leaf.
 */
static int parseSub(const char *sub, char *name, char *leaf) {
    name[0] = '\0';
    leaf[0] = '\0';
    if (sub == NULL || sub[0] == '\0')
        return 0;

    size_t n = 0;
    while (sub[n] != '\0' && sub[n] != '/')
        n++;
    if (n >= WAPP_MAX_NAME_LEN)
        return -ENAMETOOLONG;
    memcpy(name, sub, n);
    name[n] = '\0';

    if (sub[n] == '\0')
        return 0;

    const char *l = sub + n + 1;
    size_t m = 0;
    while (l[m] != '\0' && l[m] != '/')
        m++;
    if (l[m] == '/')
        return -ENOENT; /* a third segment — deeper than a leaf */
    if (m >= WAPP_LEAF_MAX)
        return -ENAMETOOLONG;
    memcpy(leaf, l, m);
    leaf[m] = '\0';
    return 0;
}

/* Look up a running wapp's state by name. Returns true and fills *out when the
 * platform reports a slot for it. */
static bool findState(const char *name, wapp_state_t *out) {
    wapp_state_t states[CONFIG_WANTED_MAX_WAPPS];
    int n = PlatformWappGetState(states, CONFIG_WANTED_MAX_WAPPS);
    for (int i = 0; i < n; i++) {
        if (strncmp(states[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            *out = states[i];
            return true;
        }
    }
    return false;
}

static bool leafKnown(const char *leaf) {
    for (size_t i = 0; i < N_LEAVES; i++) {
        if (strcmp(leaf, LEAVES[i]) == 0)
            return true;
    }
    return false;
}

/* Render a leaf's plain-text value for a running wapp into out. Returns the
 * byte length (excluding NUL). */
static int renderLeaf(const char *leaf, const wapp_state_t *st, char *out,
                      size_t cap) {
    if (strcmp(leaf, "state") == 0)
        return snprintf(out, cap, "%s", StatusToString(st->status));
    if (strcmp(leaf, "image") == 0)
        return snprintf(out, cap, "%s", st->image);
    if (strcmp(leaf, "version") == 0)
        return snprintf(out, cap, "%s", st->version);
    if (strcmp(leaf, "id") == 0)
        return snprintf(out, cap, "%u", (unsigned int)st->id);
    if (strcmp(leaf, "exit_code") == 0)
        return snprintf(out, cap, "%d", (int)st->exit_code);
    if (strcmp(leaf, "memory") == 0)
        return snprintf(out, cap,
                        "linear_cur:\t%zu B\n"
                        "linear_max:\t%zu B\n"
                        "pages_cur:\t%u\n"
                        "pages_max:\t%u\n",
                        st->mem_bytes_cur, st->mem_bytes_max,
                        (unsigned int)st->mem_pages_cur,
                        (unsigned int)st->mem_pages_max);
    return -ENOENT;
}

static int wappsStat(vfs_ctx_t c, const char *sub, vfs_filetype_t *type) {
    (void)c;
    char name[WAPP_MAX_NAME_LEN];
    char leaf[WAPP_LEAF_MAX];
    int r = parseSub(sub, name, leaf);
    if (r < 0)
        return r;

    if (name[0] == '\0') {
        *type = VFS_FILETYPE_DIRECTORY; /* /proc/wapps */
        return 0;
    }

    wapp_state_t st;
    if (!findState(name, &st))
        return -ENOENT;

    if (leaf[0] == '\0') {
        *type = VFS_FILETYPE_DIRECTORY; /* /proc/wapps/<name> */
        return 0;
    }
    if (!leafKnown(leaf))
        return -ENOENT;
    *type = VFS_FILETYPE_REGULAR_FILE;
    return 0;
}

static int wappsRead(vfs_ctx_t c, const char *sub, void *buf, size_t bufLen) {
    (void)c;
    char name[WAPP_MAX_NAME_LEN];
    char leaf[WAPP_LEAF_MAX];
    int r = parseSub(sub, name, leaf);
    if (r < 0)
        return r;
    if (name[0] == '\0' || leaf[0] == '\0')
        return -EISDIR;

    wapp_state_t st;
    if (!findState(name, &st))
        return -ENOENT;

    char line[WAPP_READ_MAX];
    int n = renderLeaf(leaf, &st, line, sizeof(line));
    if (n < 0)
        return n;
    if ((size_t)n > sizeof(line) - 1)
        n = (int)(sizeof(line) - 1);
    if ((size_t)n > bufLen)
        n = (int)bufLen;
    memcpy(buf, line, (size_t)n);
    return n;
}

static int wappsReadDir(vfs_ctx_t c, const char *sub, void *buf, size_t bufLen,
                        uint64_t *cookie, size_t *bufUsed) {
    (void)c;
    char name[WAPP_MAX_NAME_LEN];
    char leaf[WAPP_LEAF_MAX];
    int r = parseSub(sub, name, leaf);
    if (r < 0)
        return r;
    if (leaf[0] != '\0')
        return -ENOTDIR; /* a leaf has no children */

    if (name[0] == '\0') {
        /* /proc/wapps — one directory per running wapp. */
        wapp_state_t states[CONFIG_WANTED_MAX_WAPPS];
        int n = PlatformWappGetState(states, CONFIG_WANTED_MAX_WAPPS);
        if (n < 0)
            return n;
        vfs_dir_entry_t entries[CONFIG_WANTED_MAX_WAPPS];
        for (int i = 0; i < n; i++) {
            entries[i].name = states[i].name;
            entries[i].type = VFS_FILETYPE_DIRECTORY;
        }
        return VfsFlatDirReadDir(entries, (size_t)n, buf, bufLen, cookie,
                                 bufUsed);
    }

    /* /proc/wapps/<name> — the fixed leaf set, only for a running wapp. */
    wapp_state_t st;
    if (!findState(name, &st))
        return -ENOENT;
    vfs_dir_entry_t entries[N_LEAVES];
    for (size_t i = 0; i < N_LEAVES; i++) {
        entries[i].name = LEAVES[i];
        entries[i].type = VFS_FILETYPE_REGULAR_FILE;
    }
    return VfsFlatDirReadDir(entries, N_LEAVES, buf, bufLen, cookie, bufUsed);
}

const proc_dir_ops_t WappsProcDirOps = {
    .stat = wappsStat,
    .read = wappsRead,
    .readdir = wappsReadDir,
};
