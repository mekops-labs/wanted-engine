#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vfs.h>
#include <wanted-api.h>

#define VFS_SKT_BUS 0
#define VFS_SKT_TCP 1
#define VFS_SKT_UDP 2

typedef vfs_driver_t *(*VfsInitFunction_t)(const wapp_t *wapp, uint8_t argc, const char *args[]);

vfs_driver_t *VfsNullInit       (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *Vfs9PInit         (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *VfsPlatformFsInit (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *VfsRomfsInit      (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *VfsSocketInit     (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *VfsVirtualInit    (const wapp_t *wapp, uint8_t argc, const char *args[]);
vfs_driver_t *VfsWantedInit     (const wapp_t *wapp, uint8_t argc, const char *args[]);
