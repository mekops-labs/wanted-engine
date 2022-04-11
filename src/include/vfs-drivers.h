#pragma once

#include <vfs.h>
#include <stdint.h>
#include <stddef.h>

#define VFS_SKT_BUS 0
#define VFS_SKT_TCP 1
#define VFS_SKT_UDP 2

vfs_driver_t *VfsPlatformFsInit();
vfs_driver_t *VfsRomfsInit(const char *root, uint8_t *RomfsImg, size_t RomfsImgLen);
vfs_driver_t *VfsSocketInit(uint8_t type, char *addr, uint16_t port);
vfs_driver_t *VfsVirtualInit();
vfs_driver_t *VfsWantedInit();
