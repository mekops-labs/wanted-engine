/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <vfs-procfs.h>
#include <vfs.h>

#define TRY_DRV(drv_ptr, oper, ...)                                            \
    (((drv_ptr)->oper != NULL) ? (drv_ptr)->oper((drv_ptr)->ctx, __VA_ARGS__)  \
                               : -EPERM)
#define TRY_FILETYPE(drv_ptr)                                                  \
    (((drv_ptr) != NULL) ? (drv_ptr)->filetype : VFS_FILETYPE_UNKNOWN)
#define TRY_ID(drv_ptr) (((drv_ptr) != NULL) ? (drv_ptr)->bytesId : 0)

#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define MAX_ENTRY_NAME_LEN 32
#define ROOT_FD 3
#define MAX_DRIVERS 10

/* Typed FD table — the only dispatch path. It sits on vfs_ctx_t, is indexed by
 * the WASI fd, and its type picks the owning subsystem. A STREAM fd carries an
 * embedded driver pointer and internal fd, so stdio stays driver-backed. */
#define VFS_MAX_FDS 32

/* Maximum length (including NUL) of a path stored on a vfs_fd_t.
 * Override in platform/include/config-*.h if needed. */
#ifndef VFS_FD_PATH_LEN
#define VFS_FD_PATH_LEN 64
#endif

#define VFS_MAX_MOUNTS 8

typedef enum {
    VFS_TYPE_NONE = 0,
    VFS_TYPE_TARFS,
    VFS_TYPE_DEV,
    VFS_TYPE_NET,
    VFS_TYPE_PROC,
    VFS_TYPE_STREAM,
    /* Host-backed fd reached via a WASI preopen; all ops delegate to a driver
     * using drv_fd as the host kernel fd. OpenAt against a PLATFORM fd bypasses
     * the mount table, since the driver resolves relative paths itself. */
    VFS_TYPE_PLATFORM,
    /* A single file/backend driver bound at an arbitrary mount prefix (e.g. a
     * config-map at "/etc/config"). The mount entry owns the driver; ops
     * delegate through it using drv_fd as the driver-internal fd. */
    VFS_TYPE_DRIVER,
    /* Synthetic intermediate directory: a path that is a strict ancestor of a
     * mount without being one. It has no backing driver; stat reports a
     * directory and readdir enumerates the components beneath it. */
    VFS_TYPE_MOUNTDIR,
} vfs_fd_type_t;

typedef struct vfs_fd_t {
    vfs_fd_type_t type;
    void *internal_ctx; /* tarfs_file_ctx_t, devfs handle, socket_ctx, ... */
    int flags;          /* VFS_O_* from open() */
    int rights;         /* WASI-style capability mask (reserved) */
    char path[VFS_FD_PATH_LEN]; /* absolute path this fd was opened at */

    /* STREAM-only: stdio backed by a driver. The driver is owned by the vfs
     * ctx — VfsDestroy walks STREAM slots and calls Destroy. */
    const vfs_driver_t *driver;
    int drv_fd;
} vfs_fd_t;

typedef struct vfs_entry_t {
    char name[MAX_ENTRY_NAME_LEN];
    const vfs_driver_t *drv;
} vfs_entry_t;

/* Forward-declared here so the ctx can carry a per-wapp tarfs pointer
 * without pulling vfs-tarfs.h into every translation unit. */
struct vfs_tarfs_ctx_t;

/* Direct DevFs/NetFs registration tables, filled by WantedInstallDriver. The
 * prefix router resolves an open by exact-matching a suffix against `name`. */
#define VFS_DEVFS_MAX_ENTRIES 10

/* ProcFS registration table — flat read-only entries under "/proc". */
#define VFS_PROCFS_MAX_ENTRIES 16

typedef struct vfs_proc_entry_t {
    char name[MAX_ENTRY_NAME_LEN];
    proc_read_fn_t read_fn;        /* flat file reader; NULL for a directory */
    const proc_dir_ops_t *dir_ops; /* directory ops; NULL for a flat file */
    bool privileged;
} vfs_proc_entry_t;

typedef struct vfs_named_drv_t {
    char name[MAX_ENTRY_NAME_LEN];
    const vfs_driver_t *drv;
} vfs_named_drv_t;

/* Mount prefix length — sized for an arbitrary absolute mount path such as
 * "/etc/config", not just the fixed top-level namespaces. */
#define VFS_MOUNT_PREFIX_LEN 64

/* Mount table entry — drives route_open dispatch and root VfsReadDir listing.
 * For VFS_TYPE_DRIVER mounts `drv` is the bound driver, owned by the mount and
 * destroyed on VfsDestroy; it is NULL for the fixed namespace types. */
typedef struct vfs_mount_t {
    char prefix[VFS_MOUNT_PREFIX_LEN]; /* "/", "/dev", "/net", "/proc",
                                          "/etc/config" */
    vfs_fd_type_t type;
    const vfs_driver_t *drv;
} vfs_mount_t;

struct vfs_ctx_t {
    /* Typed FD table — owns every open in the wapp. */
    vfs_fd_t fds[VFS_MAX_FDS];
    struct vfs_tarfs_ctx_t *tarfs;

    /* Direct DevFs/NetFs registration tables. Owned by the vfs ctx;
     * VfsDestroy walks both and calls each driver's Destroy. */
    vfs_named_drv_t devfs[VFS_DEVFS_MAX_ENTRIES];
    uint8_t devfs_cnt;
    vfs_named_drv_t netfs[VFS_DEVFS_MAX_ENTRIES];
    uint8_t netfs_cnt;

    /* Mount table — populated by VfsInit; drives routing and root listing. */
    vfs_mount_t mounts[VFS_MAX_MOUNTS];
    uint8_t mounts_cnt;

    /* ProcFS registration table — populated at wapp-setup time. */
    vfs_proc_entry_t procfs[VFS_PROCFS_MAX_ENTRIES];
    uint8_t procfs_cnt;

    /* System-level privilege flag — enables privileged /proc entries. */
    bool privileged;

    /* Raised to end a blocking wait when the wapp is stopping; -1 when the
     * platform interrupts by signal. Owned here, closed by VfsDestroy. */
    int wake_fd;
};

/* Flat-directory readdir helper — shared by DevFS root, NetFS root, ProcFS.
 * Iterates `entries[0..count)`, packing vfs_dirent_t structs into buf using
 * *cookie as the resume index. Sets *bufUsed on return. */
typedef struct vfs_dir_entry_t {
    const char *name;
    vfs_filetype_t type;
} vfs_dir_entry_t;

int VfsFlatDirReadDir(const vfs_dir_entry_t *entries, size_t count, void *buf,
                      size_t bufLen, uint64_t *cookie, size_t *bufUsed);

int VfsFindEntry(const char *path, const vfs_entry_t *files,
                 const char **pathLeft);
