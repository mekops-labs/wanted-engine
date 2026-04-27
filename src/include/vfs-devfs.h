#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/* DevFs — Phase 4 prefix-router shim for "/dev/<name>" paths.
 *
 * Phase 4 keeps the legacy per-wapp driver registration intact: drivers are
 * still installed under the rootDriver (virt) subtree by WantedInstallDriver.
 * DevFs sits in front of that registration and exposes a typed-FD API the
 * stateless prefix router can call into. Phase 6 will replace the forwarding
 * body with a direct, table-backed lookup once wanted.c stops registering
 * drivers through virt.
 */

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
