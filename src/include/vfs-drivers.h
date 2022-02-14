#pragma once

#include <vfs.h>
#include <stdint.h>
#include <stddef.h>

int VfsVirtualInit(vfs_driver_t *driver);
void VfsVirtualDestroy(vfs_driver_t *driver);

int VfsRomfsInit(vfs_driver_t *driver, const char *root, uint8_t *RomfsImg, size_t RomfsImgLen);
void VfsRomfsDestroy(vfs_driver_t *driver);
