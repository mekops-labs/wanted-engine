/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>
#include <wanted-api.h>

#ifndef SECURE_SOCKETS
#define SECURE_SOCKETS 0
#endif

enum vfs_socket_type_t {
    VFS_SKT_BUS,
    VFS_SKT_TCP,
    VFS_SKT_UDP,
    VFS_SKT_STCP, /* secure tcp */
    VFS_SKT_SUDP, /* secure udp */
};

typedef vfs_driver_t *(*VfsInitFunction_t)(const wapp_t *wapp,
                                           const char *options);

typedef struct vfs_driver_table_t {
    char *name;
    VfsInitFunction_t init;
} vfs_driver_table_t;

vfs_driver_t *VfsNullInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsLogInit(const wapp_t *wapp, const char *options);
vfs_driver_t *Vfs9PInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsConfigInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsPlatformFsInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsSocketInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsVirtualInit(const wapp_t *wapp, const char *options);
vfs_driver_t *VfsWantedInit(const wapp_t *wapp, const char *options);
