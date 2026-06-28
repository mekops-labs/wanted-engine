/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-procfs.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* ProcFS — read-only virtual filesystem under "/proc".
 *
 * Each entry is registered at wapp-setup time as one of two shapes:
 *   - a flat file (name, read_fn): /proc/<name>, read in one shot;
 *   - a directory (name, dir_ops): /proc/<name>/... whose children are
 *     resolved dynamically by the ops table (e.g. /proc/wapps/<wapp>/<leaf>).
 * Privileged entries are hidden and return -EACCES unless vfs_ctx_t.privileged
 * is set. A file read_fn / dir_ops->read fills the caller's buffer in one call;
 * the handle latches that the call was made so subsequent reads return 0 (EOF).
 */

/* Holds an entry's sub-path for a directory open: "" (the entry dir itself),
 * "<wapp>", or "<wapp>/<leaf>". Bounds the two path segments below the entry
 * name. */
#define PROCFS_SUB_MAX (2 * MAX_ENTRY_NAME_LEN)

typedef struct procfs_handle_t {
    const vfs_proc_entry_t *entry; /* NULL for root directory */
    bool is_root;
    bool is_dir;    /* directory node (root, or a dir-entry subdir) */
    bool read_done; /* one-shot EOF latch for file reads */
    char sub[PROCFS_SUB_MAX]; /* sub-path under a directory entry */
} procfs_handle_t;

/* Claim a fresh entry slot for `name`, sharing the validation and duplicate
 * check across the file and directory registrars. Returns the slot or NULL,
 * writing the error to *err. */
static vfs_proc_entry_t *claimEntry(vfs_ctx_t c, const char *name, int *err) {
    if (!c || !name || *name == '\0') {
        *err = -EINVAL;
        return NULL;
    }
    if (strlen(name) >= MAX_ENTRY_NAME_LEN) {
        *err = -ENAMETOOLONG;
        return NULL;
    }
    if (c->procfs_cnt >= VFS_PROCFS_MAX_ENTRIES) {
        *err = -ENOSPC;
        return NULL;
    }
    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (strncmp(c->procfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0) {
            *err = -EEXIST;
            return NULL;
        }
    }

    vfs_proc_entry_t *e = &c->procfs[c->procfs_cnt++];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, name, MAX_ENTRY_NAME_LEN - 1);
    e->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
    *err = 0;
    return e;
}

int ProcFs_Register(vfs_ctx_t c, const char *name, proc_read_fn_t fn,
                    bool privileged) {
    if (!fn)
        return -EINVAL;
    int err;
    vfs_proc_entry_t *e = claimEntry(c, name, &err);
    if (!e)
        return err;
    e->read_fn = fn;
    e->privileged = privileged;
    DEBUG_TRACE("/proc/%s (privileged=%d)", e->name, privileged);
    return 0;
}

int ProcFs_RegisterDir(vfs_ctx_t c, const char *name, const proc_dir_ops_t *ops,
                       bool privileged) {
    if (!ops || !ops->stat || !ops->read || !ops->readdir)
        return -EINVAL;
    int err;
    vfs_proc_entry_t *e = claimEntry(c, name, &err);
    if (!e)
        return err;
    e->dir_ops = ops;
    e->privileged = privileged;
    DEBUG_TRACE("/proc/%s/ (dir, privileged=%d)", e->name, privileged);
    return 0;
}

void ProcFs_Destroy(vfs_ctx_t c) {
    if (!c)
        return;
    c->procfs_cnt = 0;
}

/* Split `suffix` into its first path segment and the remainder. `seg` receives
 * the first segment (bounded by MAX_ENTRY_NAME_LEN); *rest points at the tail
 * after the separating '/', or "" when there is none. Returns -ENAMETOOLONG if
 * the first segment does not fit. A leading '/' is not expected here — the VFS
 * router hands ProcFS the path below "/proc/". */
static int splitFirst(const char *suffix, char *seg, const char **rest) {
    size_t n = 0;
    while (suffix[n] != '\0' && suffix[n] != '/')
        n++;
    if (n >= MAX_ENTRY_NAME_LEN)
        return -ENAMETOOLONG;
    memcpy(seg, suffix, n);
    seg[n] = '\0';
    *rest = (suffix[n] == '/') ? suffix + n + 1 : suffix + n;
    return 0;
}

static vfs_proc_entry_t *findEntry(vfs_ctx_t c, const char *name) {
    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (strncmp(c->procfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return &c->procfs[i];
    }
    return NULL;
}

void *ProcFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                  int *out_err) {
    (void)flags;
    DEBUG_TRACE("/proc/%s (0x%x)", suffix ? suffix : "(null)", flags);

    int err = 0;
    procfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        if (out_err)
            *out_err = -ENOMEM;
        return NULL;
    }
    memset(h, 0, sizeof(*h));

    if (!suffix || *suffix == '\0') {
        h->is_root = true;
        h->is_dir = true;
        goto ok;
    }

    char seg[MAX_ENTRY_NAME_LEN];
    const char *rest;
    err = splitFirst(suffix, seg, &rest);
    if (err < 0)
        goto fail;

    vfs_proc_entry_t *e = findEntry(c, seg);
    if (!e) {
        err = -ENOENT;
        goto fail;
    }
    if (e->privileged && !c->privileged) {
        err = -EACCES;
        goto fail;
    }

    if (e->dir_ops) {
        /* Directory entry: the ops table resolves the sub-path's existence and
         * kind. rest == "" addresses the entry's own directory. */
        vfs_filetype_t type;
        err = e->dir_ops->stat(c, rest, &type);
        if (err < 0)
            goto fail;
        h->entry = e;
        h->is_dir = (type == VFS_FILETYPE_DIRECTORY);
        strncpy(h->sub, rest, PROCFS_SUB_MAX - 1);
        h->sub[PROCFS_SUB_MAX - 1] = '\0';
        goto ok;
    }

    /* Flat file entry: it has no children, so any trailing path is ENOENT. */
    if (*rest != '\0') {
        err = -ENOENT;
        goto fail;
    }
    h->entry = e;
    goto ok;

ok:
    if (out_err)
        *out_err = 0;
    return h;
fail:
    WantedFree(h);
    if (out_err)
        *out_err = err;
    return NULL;
}

int ProcFs_Close(vfs_ctx_t c, void *handle) {
    (void)c;
    procfs_handle_t *h = handle;
    if (!h)
        return -EBADF;
    WantedFree(h);
    return 0;
}

int ProcFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte) {
    procfs_handle_t *h = handle;
    if (!h)
        return -EBADF;
    if (h->is_dir)
        return -EISDIR;
    if (h->read_done)
        return 0;
    int n = h->entry->dir_ops ? h->entry->dir_ops->read(c, h->sub, buf, nbyte)
                              : h->entry->read_fn(c, buf, nbyte);
    if (n >= 0)
        h->read_done = true;
    return n;
}

int ProcFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat) {
    (void)c;
    procfs_handle_t *h = handle;
    if (!h)
        return -EBADF;
    memset(stat, 0, sizeof(*stat));
    stat->filetype =
        h->is_dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_REGULAR_FILE;
    return 0;
}

int ProcFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed) {
    const procfs_handle_t *h = handle;
    if (!h || !h->is_dir)
        return -EBADF;

    /* A directory entry below the root delegates to its ops, which enumerate
     * the children of the sub-path the handle was opened on. */
    if (!h->is_root)
        return h->entry->dir_ops->readdir(c, h->sub, buf, bufLen, cookie,
                                          bufUsed);

    /* Root: list the registered entries, omitting privileged ones when not
     * allowed. Directory entries advertise their type so a caller can recurse.
     */
    vfs_dir_entry_t entries[VFS_PROCFS_MAX_ENTRIES];
    size_t count = 0;
    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (c->procfs[i].privileged && !c->privileged)
            continue;
        entries[count].name = c->procfs[i].name;
        entries[count].type = c->procfs[i].dir_ops ? VFS_FILETYPE_DIRECTORY
                                                    : VFS_FILETYPE_REGULAR_FILE;
        count++;
    }
    return VfsFlatDirReadDir(entries, count, buf, bufLen, cookie, bufUsed);
}
