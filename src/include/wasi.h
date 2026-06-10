/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>
#include <vfs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WASI_MAX_PREOPENS 8

/* Per-wapp preopen entry. `fd` is the VFS fd assigned at bind time; -1 means
 * the entry is lazy and will be opened on first fd_prestat_get(). The path
 * is what the wapp sees via fd_prestat_dir_name. */
typedef struct wasi_preopen_t {
    char path[64];
    int  fd;
} wasi_preopen_t;

typedef struct wasi_ctx_t {
    int32_t     exit_code;
    uint32_t    argc;
    const char **argv;
    uint32_t    envc;
    const char **envp;
    vfs_ctx_t   vfsCtx;
    wasi_preopen_t preopens[WASI_MAX_PREOPENS];
    uint8_t        preopens_cnt;
} wasi_ctx_t;

void RegisterWASINatives(void);

wasi_ctx_t *InitWasiContext(void);
void FreeWasiContext(wasi_ctx_t *);

/* Append a preopen for `path`, backed by an already-opened host directory fd.
 * The Engine must open the host directory before launching wasm. The new VFS
 * fd is allocated from the wasi ctx's vfs and stored in the returned entry.
 * Returns 0 on success or a negative errno. */
int WasiCtxAddPreopen(wasi_ctx_t *ctx, const char *path, int host_fd);

#ifdef __cplusplus
}
#endif
