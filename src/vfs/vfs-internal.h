#pragma once

#include <stdint.h>
#include <vfs.h>
#include <stdbool.h>

#define TRY_DRV(drv_ptr,oper,...) (((drv_ptr)->oper != NULL) ? (drv_ptr)->oper( (drv_ptr)->ctx, __VA_ARGS__ ) : -EPERM)
#define TRY_FILETYPE(drv_ptr) (((drv_ptr) != NULL) ? (drv_ptr)->filetype : VFS_FILETYPE_UNKNOWN)
#define TRY_ID(drv_ptr) (((drv_ptr) != NULL) ? (drv_ptr)->bytesId : 0)

#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define MAX_ENTRY_NAME_LEN  32
#define MAX_OPEN            20
#define ROOT_FD             3
#define MAX_DRIVERS         10

typedef struct vfs_entry_t {
    char                name[MAX_ENTRY_NAME_LEN];
    const vfs_driver_t  *drv;
//    vfs_filetype_t  type;
} vfs_entry_t;

typedef struct vfs_fildes_t {
    int                 drv_fd;
    const vfs_driver_t  *drv;
    bool                opened;
    int                 flags;
} vfs_fildes_t;

struct vfs_ctx_t {
    vfs_fildes_t fildes[MAX_OPEN];
    const vfs_driver_t *drivers[MAX_DRIVERS];
    size_t nDrivers;
};

int VfsFindEntry(const char *path, vfs_entry_t *files, const char **pathLeft);
