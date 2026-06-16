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

/* ProcFS — flat read-only virtual filesystem under "/proc".
 *
 * Each entry is a (name, read_fn, privileged) tuple registered at wapp-setup
 * time. Privileged entries are hidden and return -EACCES unless
 * vfs_ctx_t.privileged is set. The read_fn fills the caller's buffer in one
 * call; the handle tracks whether that call has been made so subsequent reads
 * return 0 (EOF). */

typedef struct procfs_handle_t {
    const vfs_proc_entry_t *entry; /* NULL for root directory */
    bool is_root;
    bool read_done;
} procfs_handle_t;

int ProcFs_Register(vfs_ctx_t c, const char *name, proc_read_fn_t fn,
                    bool privileged) {
    if (!c || !name || *name == '\0' || !fn)
        return -EINVAL;
    if (strlen(name) >= MAX_ENTRY_NAME_LEN)
        return -ENAMETOOLONG;
    if (c->procfs_cnt >= VFS_PROCFS_MAX_ENTRIES)
        return -ENOSPC;

    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (strncmp(c->procfs[i].name, name, MAX_ENTRY_NAME_LEN) == 0)
            return -EEXIST;
    }

    vfs_proc_entry_t *e = &c->procfs[c->procfs_cnt++];
    strncpy(e->name, name, MAX_ENTRY_NAME_LEN - 1);
    e->name[MAX_ENTRY_NAME_LEN - 1] = '\0';
    e->read_fn = fn;
    e->privileged = privileged;
    DEBUG_TRACE("/proc/%s (privileged=%d)", e->name, privileged);
    return 0;
}

void ProcFs_Destroy(vfs_ctx_t c) {
    if (!c)
        return;
    c->procfs_cnt = 0;
}

void *ProcFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                  int *out_err) {
    (void)flags;
    DEBUG_TRACE("/proc/%s (0x%x)", suffix ? suffix : "(null)", flags);

    procfs_handle_t *h = WantedMalloc(sizeof(*h));
    if (!h) {
        if (out_err)
            *out_err = -ENOMEM;
        return NULL;
    }

    if (!suffix || *suffix == '\0') {
        h->entry = NULL;
        h->is_root = true;
        h->read_done = false;
        if (out_err)
            *out_err = 0;
        return h;
    }

    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (strncmp(c->procfs[i].name, suffix, MAX_ENTRY_NAME_LEN) == 0) {
            if (c->procfs[i].privileged && !c->privileged) {
                WantedFree(h);
                if (out_err)
                    *out_err = -EACCES;
                return NULL;
            }
            h->entry = &c->procfs[i];
            h->is_root = false;
            h->read_done = false;
            if (out_err)
                *out_err = 0;
            return h;
        }
    }

    WantedFree(h);
    if (out_err)
        *out_err = -ENOENT;
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
    if (!h || h->is_root)
        return -EBADF;
    if (h->read_done)
        return 0;
    int n = h->entry->read_fn(c, buf, nbyte);
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
        h->is_root ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_REGULAR_FILE;
    return 0;
}

int ProcFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed) {
    const procfs_handle_t *h = handle;
    if (!h || !h->is_root)
        return -EBADF;

    /* Build entry list on stack, omitting privileged entries when not allowed.
     */
    vfs_dir_entry_t entries[VFS_PROCFS_MAX_ENTRIES];
    size_t count = 0;
    for (uint8_t i = 0; i < c->procfs_cnt; i++) {
        if (c->procfs[i].privileged && !c->privileged)
            continue;
        entries[count].name = c->procfs[i].name;
        entries[count].type = VFS_FILETYPE_REGULAR_FILE;
        count++;
    }
    return VfsFlatDirReadDir(entries, count, buf, bufLen, cookie, bufUsed);
}
