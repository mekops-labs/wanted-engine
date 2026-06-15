/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>

#include "vfs-internal.h"

#define MAX_ENTRIES 10

/* The virt driver tracks its own opens internally via a per-driver fildes[]
 * table. MAX_OPEN caps the number of concurrent virt handles per context. */
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
