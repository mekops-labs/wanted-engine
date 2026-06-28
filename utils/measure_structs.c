/* SPDX-License-Identifier: Apache-2.0 */

/* Resource-footprint measurement TU for the WANTED engine.
 *
 * Reports the byte size of the engine's per-wapp and fixed structures for the
 * resource limits it is compiled with, so each build profile
 * (cmake/profiles/<name>.cmake) can be annotated with an exact memory cost.
 *
 * It pulls the real engine headers, so the figures track the actual structs and
 * the active MAX_* / WASM_* limits — there is nothing to keep in sync by hand.
 *
 * Mechanism: each measured number is emitted as the *size of a global symbol*
 * (an uninitialised char array). A compile-only build (-c) is therefore enough
 * to read every figure back from the object's symbol table with `readelf -sW`;
 * no program is run and no target libc/runtime is required. That lets the same
 * source be compiled for the 32-bit embedded ABI with a freestanding
 * cross-target (clang -ffreestanding -target i386-...), which would otherwise
 * fail to link. utils/measure-sizes.sh drives this for every profile on both
 * the host (LP64) and 32-bit (ILP32) models; see `make sizes`.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vfs-internal.h> /* struct vfs_ctx_t — the heap-allocated VFS context */
#include <wanted-api.h> /* wapp_t, wapp_data_t, wantedConfig_t + the limits  */
#include <wasi.h>       /* wasi_ctx_t                                         */

/* wamrData_t is private to wanted.c: three opaque WAMR handles plus one byte
 * pointer — four pointer-sized members. Mirrored here for measurement. */
struct measured_wamr_t {
    void *module;
    void *instance;
    void *exec_env;
    uint8_t *wasm_bytes;
};

/* Per-wapp log ring slot, mirrored from log-store.c (LOG_CAP bytes retained per
 * wapp plus the ring bookkeeping). The store reserves LOG_SLOTS == MAX_WAPPS of
 * these, one per wapp. */
#define MEASURED_LOG_CAP 2048
struct measured_log_slot_t {
    char name[WAPP_MAX_NAME_LEN];
    char buf[MEASURED_LOG_CAP];
    size_t start;
    size_t len;
    uint64_t tick;
    bool used;
};

/* Emit one measured value as a global symbol whose size equals it. */
#define EMIT(name, value) char measured_##name[(value) > 0 ? (value) : 1]

/* Target ABI */
EMIT(ptr, sizeof(void *));
EMIT(size_t, sizeof(size_t));

/* Per-wapp structures */
EMIT(wapp_t, sizeof(wapp_t));
EMIT(wapp_data_t, sizeof(wapp_data_t));
EMIT(vfs_ctx_t, sizeof(struct vfs_ctx_t));
EMIT(wasi_ctx_t, sizeof(wasi_ctx_t));
EMIT(wamrData_t, sizeof(struct measured_wamr_t));
EMIT(log_slot_t, sizeof(struct measured_log_slot_t));

/* Engine-fixed structures */
EMIT(wantedConfig_t, sizeof(wantedConfig_t));

/* Active resource limits (so the report names the profile it measured) */
EMIT(MAX_WAPPS, MAX_WAPPS);
EMIT(MAX_PATH_LEN, MAX_PATH_LEN);
EMIT(WASM_STACK_SIZE, WASM_STACK_SIZE);
EMIT(WASM_HEAP_SIZE, WASM_HEAP_SIZE);
EMIT(WASM_WORKER_STACK_SIZE, WASM_WORKER_STACK_SIZE);
