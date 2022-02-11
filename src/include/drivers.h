#pragma once

#include <vfs.h>
#include <stdint.h>
#include <stddef.h>

int VfsRomfsInit(const char *root, uint8_t *RomfsImg, size_t RomfsImgLen, vfs_driver_t *driver);
void VfsRomfsDestroy(vfs_driver_t *driver);
