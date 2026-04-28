#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/* DevFs — prefix-router shim for "/dev/<name>" paths.
 *
 * Phase 6 owns driver lifetime directly: WantedInstallDriver calls
 * DevFs_Register to insert (name, driver) pairs into the per-wapp table on
 * vfs_ctx_t. DevFs_Open resolves "/dev/<suffix>" by exact-matching `suffix`
 * against the registered names — no more forwarding through the legacy
 * virt-rooted dispatch.
 */

int DevFs_Register(vfs_ctx_t c, const char *name, const vfs_driver_t *driver);
void DevFs_Destroy(vfs_ctx_t c);

void *DevFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags);
int DevFs_Close(vfs_ctx_t c, void *handle);
int DevFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte);
int DevFs_Write(vfs_ctx_t c, void *handle, const void *buf, size_t nbyte);
int DevFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat);
int DevFs_StatSet(vfs_ctx_t c, void *handle, vfs_stat_t stat);
int DevFs_Seek(vfs_ctx_t c, void *handle, long off, vfs_whence_t whence,
               long *pos);
int DevFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed);
int DevFs_Unlink(vfs_ctx_t c, void *handle, const char *path);
