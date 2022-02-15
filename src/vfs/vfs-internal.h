#pragma once

#include <stdint.h>
#include <vfs.h>
#include <stdbool.h>

#define TRY_DRV(drv_ptr,oper,...) (((drv_ptr)->oper != NULL) ? (drv_ptr)->oper( (drv_ptr)->ctx, __VA_ARGS__ ) : -EPERM)

#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define MAX_ENTRY_NAME_LEN  32
#define MAX_OPEN            20
#define ROOT_FD             3

typedef struct vfs_entry_t {
    char      name[MAX_ENTRY_NAME_LEN];
    vfs_driver_t    *drv;
} vfs_entry_t;

typedef struct vfs_fildes_t {
    int             drv_fd;
    vfs_driver_t    *drv;
    bool            opened;
    int             flags;
} vfs_fildes_t;

struct vfs_ctx_t {
    vfs_fildes_t fildes[MAX_OPEN];
};

int VfsFindEntryAt(int fd, const char *path, vfs_entry_t *files, const char **pathLeft);
