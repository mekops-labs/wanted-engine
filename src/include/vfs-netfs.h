#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/* NetFs — prefix-router shim for "/net/<name>" paths.
 *
 * WantedInstallDriver calls NetFs_Register to insert (name, driver) pairs into
 * the per-wapp table on vfs_ctx_t. NetFs_Open resolves "/net/<suffix>" by
 * exact-matching `suffix` against the registered names.
 */

int NetFs_Register(vfs_ctx_t c, const char *name, const vfs_driver_t *driver);
void NetFs_Destroy(vfs_ctx_t c);

void *NetFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                 int *out_err);
int NetFs_Close(vfs_ctx_t c, void *handle);
int NetFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte);
int NetFs_Write(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte);
int NetFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat);
int NetFs_SockAccept(vfs_ctx_t c, void *handle, vfs_oflags_t flags, int *newFd);
int NetFs_SockRecv(vfs_ctx_t c, void *handle, void *buf, size_t nbyte,
                   vfs_riflags_t iflags, vfs_roflags_t *oflags);
int NetFs_SockSend(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte,
                   vfs_sdflags_t flags);
int NetFs_SockShutdown(vfs_ctx_t c, void *handle, vfs_sdflags_t flags);
