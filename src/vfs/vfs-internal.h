#pragma once

#include <stdbool.h>
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

/* Typed FD table — the only dispatch path. The table sits directly on
 * vfs_ctx_t and is indexed by the WASI fd; types pick which subsystem owns
 * the slot. STREAM fds carry an embedded driver pointer + internal fd so
 * stdio can stay driver-backed. */
#define VFS_MAX_FDS 32

typedef enum {
    VFS_TYPE_NONE = 0,
    VFS_TYPE_TARFS,
    VFS_TYPE_DEV,
    VFS_TYPE_NET,
    VFS_TYPE_STREAM,
} vfs_fd_type_t;

typedef struct vfs_fd_t {
    vfs_fd_type_t type;
    void *internal_ctx; /* tarfs_file_ctx_t, devfs handle, socket_ctx, ... */
    int flags;          /* VFS_O_* from open() */
    int rights;         /* WASI-style capability mask (reserved) */

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

/* Direct DevFs/NetFs registration tables. Drivers under "/dev/<name>"
 * and "/net/<name>" are registered into these per-ctx tables by
 * WantedInstallDriver. The prefix router resolves opens by exact-matching
 * the suffix against `name`. */
#define VFS_DEVFS_MAX_ENTRIES 10

typedef struct vfs_named_drv_t {
    char name[MAX_ENTRY_NAME_LEN];
    const vfs_driver_t *drv;
} vfs_named_drv_t;

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
};

int VfsFindEntry(const char *path, vfs_entry_t *files, const char **pathLeft);
