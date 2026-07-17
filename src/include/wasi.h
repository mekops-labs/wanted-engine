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
 * is what the wapp sees via fd_prestat_dir_name.
 * `rights_base`/`rights_inheriting` are the capability grant advertised for
 * this fd and inherited beneath it; a read-only mount clears WASI_RIGHTS_WRITE.
 */
typedef struct wasi_preopen_t {
    char path[64];
    int fd;
    uint64_t rights_base;
    uint64_t rights_inheriting;
} wasi_preopen_t;

typedef struct wasi_ctx_t {
    int32_t exit_code;
    uint32_t argc;
    const char **argv;
    uint32_t envc;
    const char **envp;
    vfs_ctx_t vfsCtx;
    wasi_preopen_t preopens[WASI_MAX_PREOPENS];
    uint8_t preopens_cnt;
} wasi_ctx_t;

void RegisterWASINatives(void);

wasi_ctx_t *InitWasiContext(void);
void FreeWasiContext(wasi_ctx_t *ctx);

/* Append a preopen at the wapp-visible `path`, backed by an already-opened host
 * directory fd. `hostPath` is the real backing directory (used to label the
 * driver for debugging; it may differ from `path`). `readonly` binds the mount
 * without write capability — the backing driver rejects writes with -EROFS. The
 * Engine must open the host directory before launching wasm. The new VFS fd is
 * allocated from the wasi ctx's vfs and stored in the returned entry. Returns 0
 * on success or a negative errno. */
int WasiCtxAddPreopen(wasi_ctx_t *ctx, const char *path, const char *hostPath,
                      int host_fd, bool readonly);

/* Find the preopen owning `fd`, or NULL if `fd` is not a preopen. Never binds a
 * lazy (unopened) preopen, so it is safe where triggering a bind would be an
 * unwanted side effect (fd_fdstat_get, path_open). */
const wasi_preopen_t *WasiCtxFindPreopen(const wasi_ctx_t *ctx, int fd);

/* True when every requested right is within the grant `inheriting` allows, i.e.
 * the request holds no right the grant withholds. A read-only preopen withholds
 * the write class, so a write-open under it is not within the grant. */
bool WasiRightsWithin(uint64_t inheriting, uint64_t requested);

#ifdef __cplusplus
}
#endif
