#pragma once

#include "vfs/vfs-internal.h"

extern unsigned char test_wasi_romfs[];
extern unsigned int test_wasi_romfs_len;

extern vfs_driver_t vfs_romfs_drv;
extern vfs_driver_t vfs_linux_drv;
extern vfs_driver_t vfs_dummy_drv;

extern vfs_entry_t fildes[];
