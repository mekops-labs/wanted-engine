#pragma once

#include <stdint.h>
#include <vfs.h>
#include <stdbool.h>

#define TRY(drv_ptr,oper,...) (((drv_ptr)->oper != NULL) ? (drv_ptr)->oper( __VA_ARGS__ ) : -EPERM)

typedef struct file_t {
    const char      *name;
    uint16_t        depth;
    vfs_driver_t    *drv;
} file_t;

typedef struct vfs_entry_t {
    int             drv_fd;
    vfs_driver_t    *drv;
    bool            opened;
} vfs_entry_t;


int VfsFindEntryAt(int fd, const char *path, file_t *files, size_t filesCnt, const char **pathLeft);
