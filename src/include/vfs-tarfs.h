#pragma once

#include <stddef.h>
#include <stdint.h>

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
