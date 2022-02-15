#pragma once

#define MAX_ENTRIES 10

struct vfs_driver_ctx_t {
    vfs_entry_t entries[MAX_ENTRIES];
    size_t cnt;
    vfs_fildes_t fildes[MAX_OPEN];
};
