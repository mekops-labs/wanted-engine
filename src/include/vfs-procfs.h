/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

typedef int (*proc_read_fn_t)(vfs_ctx_t c, void *buf, size_t bufLen);

/* A directory-type ProcFS entry whose children are resolved dynamically, e.g.
 * /proc/wapps/<name>/<leaf>. `subpath` is the path below the entry name with no
 * leading slash: "" (the entry directory itself), "<name>", or "<name>/<leaf>".
 * The ProcFS driver owns paging and the privileged gate; these callbacks only
 * resolve, render, and enumerate. */
typedef struct proc_dir_ops_t {
    /* Classify subpath: set *type to VFS_FILETYPE_DIRECTORY or
     * VFS_FILETYPE_REGULAR_FILE. Return 0, or -ENOENT when subpath names
     * nothing. */
    int (*stat)(vfs_ctx_t c, const char *subpath, vfs_filetype_t *type);
    /* Render the leaf at subpath into buf in one shot. Return the byte count
     * (>=0), or -errno (-EISDIR when subpath is a directory). */
    int (*read)(vfs_ctx_t c, const char *subpath, void *buf, size_t bufLen);
    /* Enumerate the children of the directory at subpath — the same contract as
     * a driver ReadDir. Return 0 / -errno (-ENOTDIR when subpath is a leaf). */
    int (*readdir)(vfs_ctx_t c, const char *subpath, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed);
} proc_dir_ops_t;

int ProcFs_Register(vfs_ctx_t c, const char *name, proc_read_fn_t fn,
                    bool privileged);
int ProcFs_RegisterDir(vfs_ctx_t c, const char *name, const proc_dir_ops_t *ops,
                       bool privileged);
void *ProcFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                  int *out_err);
int ProcFs_Close(vfs_ctx_t c, void *handle);
int ProcFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte);
int ProcFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat);
int ProcFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed);
void ProcFs_Destroy(vfs_ctx_t c);
