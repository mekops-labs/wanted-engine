/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vfs.h>

/* Maximum number of OCI layers per wapp. Also used by wapp_t in wanted-api.h.
 */
#define TARFS_MAX_LAYERS 4

/* Opaque tarfs context — built from an ordered array of OCI TAR layers.
 * layers[0] is the topmost/newest; its entries shadow older layers. */
typedef struct vfs_tarfs_ctx_t vfs_tarfs_ctx_t;

/* Scan every layer, build the sorted deduplicated path index, and pre-fetch the
 * app.wasm entrypoint pointer for zero-redundant-scan boot.
 * Returns NULL on failure (bad args / OOM). */
vfs_tarfs_ctx_t *TarFsInit(uint8_t *const layers[], const size_t layer_lens[],
                           uint8_t layer_cnt);

void TarFsDestroy(vfs_tarfs_ctx_t *ctx);

/* Hand a tarfs context to a vfs ctx so the prefix router resolves non-/dev,
 * non-/net paths against it. The vfs ctx takes ownership and TarFsDestroy()s
 * it. Returns -EINVAL on a NULL vfs ctx. */
int VfsAttachTarfs(vfs_ctx_t c, vfs_tarfs_ctx_t *tarfs);

/* Pre-fetched boot entrypoint. Returns NULL if app.wasm was absent or shadowed
 * by a whiteout in the top layer. */
const uint8_t *TarFsEntrypointWasm(const vfs_tarfs_ctx_t *ctx, size_t *len);

/* Indexed entry count — exposed for tests and diagnostics. */
uint16_t TarFsIndexLen(const vfs_tarfs_ctx_t *ctx);

/* File/directory operations. TarFs is read-only; Open returns a heap-owned
 * opaque handle or NULL. Directories are implicit: the sorted index encodes
 * the tree by path prefix, so typeflag '5' entries are dropped when indexed. */

void *TarFs_Open(const vfs_tarfs_ctx_t *ctx, const char *path,
                 vfs_oflags_t flags);
int TarFs_Close(vfs_tarfs_ctx_t *ctx, void *handle);
int TarFs_Read(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t nbyte);
int TarFs_Stat(vfs_tarfs_ctx_t *ctx, void *handle, vfs_stat_t *stat);
int TarFs_Seek(vfs_tarfs_ctx_t *ctx, void *handle, long off,
               vfs_whence_t whence, long *pos);
int TarFs_ReadDir(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed);
