#pragma once

#include <vfs.h>

int  VfsLinuxInit(vfs_driver_t *driver, const char *root);
void VfsLinuxDestroy(vfs_driver_t *driver);
