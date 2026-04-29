#pragma once

#include <stdbool.h>

#include "vfs-internal.h"

// TODO: need to by more dynamic and support a tree of file entries
#define MAX_ENTRIES 10

/* Phase 8: virt is the sole remaining consumer of the per-driver fildes[]
 * pattern; the global VFS dispatch table moved to a typed FD model on
 * vfs_ctx_t. We keep these here because the virt driver still tracks its
 * own opens internally. */
#define MAX_OPEN 20

typedef struct vfs_fildes_t {
    int drv_fd;
    const vfs_driver_t *drv;
    bool opened;
    int flags;
} vfs_fildes_t;

struct vfs_driver_ctx_t {
    vfs_entry_t entries[MAX_ENTRIES];
    size_t cnt;
    vfs_fildes_t fildes[MAX_OPEN];
};
