#pragma once

#include <vfs.h>

vfs_driver_t *VfsStdinDriverGet(void);
vfs_driver_t *VfsStdoutDriverGet(void);
vfs_driver_t *VfsStderrDriverGet(void);
