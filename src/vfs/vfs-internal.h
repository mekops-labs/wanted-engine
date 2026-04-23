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
#define MAX_OPEN 20
#define ROOT_FD 3
#define MAX_DRIVERS 10

/* Typed FD table — Phase 3. Populated by Phase 4's stateless prefix router. */
#define VFS_MAX_FDS 32

typedef enum {
    VFS_TYPE_NONE = 0,
    VFS_TYPE_TARFS,
    VFS_TYPE_DEV,
    VFS_TYPE_NET,
} vfs_fd_type_t;

typedef struct vfs_fd_t {
    vfs_fd_type_t type;
    void *internal_ctx; /* tarfs_file_ctx_t, devfs handle, socket_ctx, ... */
    int flags;          /* VFS_O_* from open() */
    int rights;         /* WASI-style capability mask (reserved) */
} vfs_fd_t;

typedef struct vfs_entry_t {
    char name[MAX_ENTRY_NAME_LEN];
    const vfs_driver_t *drv;
    //    vfs_filetype_t  type;
} vfs_entry_t;

typedef struct vfs_fildes_t {
    int drv_fd;
    const vfs_driver_t *drv;
    bool opened;
    int flags;
} vfs_fildes_t;

/* Forward-declared here so the ctx can carry a per-wapp tarfs pointer
 * without pulling vfs-tarfs.h into every translation unit. */
struct vfs_tarfs_ctx_t;

struct vfs_ctx_t {
    /* Legacy per-driver FD table — removed in Phase 4. */
    vfs_fildes_t fildes[MAX_OPEN];
    const vfs_driver_t *rootDriver;

    /* New typed FD table — owned by the VFS core, populated in Phase 4. */
    vfs_fd_t fds[VFS_MAX_FDS];
    struct vfs_tarfs_ctx_t *tarfs;
};

int VfsFindEntry(const char *path, vfs_entry_t *files, const char **pathLeft);
