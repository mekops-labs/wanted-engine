#pragma once

#include <vfs.h>

vfs_driver_t *VfsLinuxInit(const wapp_t *wapp, uint8_t argc, const char *args[]);
