#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vfs.h>
#include <wanted-api.h>

/* Opaque tarfs context — built from an ordered array of OCI TAR layers.
 * layers[0] is the topmost/newest; its entries shadow older layers. */
typedef struct vfs_tarfs_ctx_t vfs_tarfs_ctx_t;

/* Scan every layer, build the sorted deduplicated path index, pre-fetch
 * app.wasm and manifest.json pointers for zero-redundant-scan boot.
 * Returns NULL on failure (bad args / OOM). */
vfs_tarfs_ctx_t *TarFsInit(uint8_t *const layers[], const size_t layer_lens[],
                           uint8_t layer_cnt);

void TarFsDestroy(vfs_tarfs_ctx_t *ctx);

/* Pre-fetched boot entrypoints. Returns NULL if the path was absent or
 * shadowed by a whiteout in the top layer. */
const uint8_t *TarFsEntrypointWasm(const vfs_tarfs_ctx_t *ctx, size_t *len);
const uint8_t *TarFsEntrypointManifest(const vfs_tarfs_ctx_t *ctx, size_t *len);

/* Indexed entry count — exposed for tests and diagnostics. */
uint16_t TarFsIndexLen(const vfs_tarfs_ctx_t *ctx);

/* Phase 5 file/directory operations.
 *
 * TarFs is read-only. TarFs_Open returns a heap-owned handle on success or
 * NULL on -ENOENT / shadowed-by-whiteout / write flags. The handle is
 * opaque — pass it back into the matching op until TarFs_Close releases it.
 *
 * Directories are *implicit*: the indexer drops typeflag '5' entries because
 * the sorted index already encodes the tree by path prefix. TarFs_Open on
 * "/" or any prefix that is shared by at least one file returns a directory
 * handle; TarFs_ReadDir walks the index forward emitting immediate children
 * and synthesised subdirectories. */

void *TarFs_Open(vfs_tarfs_ctx_t *ctx, const char *path, vfs_oflags_t flags);
int TarFs_Close(vfs_tarfs_ctx_t *ctx, void *handle);
int TarFs_Read(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t nbyte);
int TarFs_Stat(vfs_tarfs_ctx_t *ctx, void *handle, vfs_stat_t *stat);
int TarFs_Seek(vfs_tarfs_ctx_t *ctx, void *handle, long off,
               vfs_whence_t whence, long *pos);
int TarFs_ReadDir(vfs_tarfs_ctx_t *ctx, void *handle, void *buf, size_t bufLen,
                  uint64_t *cookie, size_t *bufUsed);
