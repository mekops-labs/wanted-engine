#pragma once

#include <vfs.h>
#include <stdint.h>
#include <stddef.h>

#define VFS_SKT_BUS 0
#define VFS_SKT_TCP 1
#define VFS_SKT_UDP 2

int VfsVirtualInit(vfs_driver_t *driver);
void VfsVirtualDestroy(vfs_driver_t *driver);

int VfsRomfsInit(vfs_driver_t *driver, const char *root, uint8_t *RomfsImg, size_t RomfsImgLen);
void VfsRomfsDestroy(vfs_driver_t *driver);

int VfsSocketInit(vfs_driver_t *driver, uint8_t type, char *addr, uint16_t port);
void VfsSocketDestroy(vfs_driver_t *driver);

int VfsWantedInit(vfs_driver_t *driver, vfs_driver_t *fileDriver);
void VfsWantedDestroy(vfs_driver_t *driver);
