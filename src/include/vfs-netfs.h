#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/* NetFs — Phase 4 prefix-router shim for "/net/<name>" paths.
 *
 * Wraps the existing socket driver (currently registered via the legacy
 * virt-rooted dispatch) so VfsSock* operations can route through the typed-FD
 * table. Phase 6 will collapse the forwarding into a direct call once the
 * socket driver is owned per-wapp outside of virt.
 */

void *NetFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags);
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
