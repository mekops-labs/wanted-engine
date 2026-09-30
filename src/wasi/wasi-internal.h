/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <vfs.h>

#include "wasi_types.h"

/* A negative VFS errno as the WASI errno a guest sees. */
__wasi_errno_t WasiErrno(int errnum);

/* True when `name` is registered in the WASI namespace `ns`
 * ("wasi_snapshot_preview1" or "wasi_unstable"). */
bool WasiHasNative(const char *ns, const char *name);

/* A platform fill: writes up to `len` bytes into `buf` and returns how many,
 * or a negative errno. */
typedef int64_t (*wasi_random_fn)(uint8_t *buf, size_t len);

/* Fill `buf` through `fill`, asking again after a short fill. A fill that
 * writes nothing is an I/O error, so a broken platform cannot hang a guest. */
__wasi_errno_t WasiRandomFill(wasi_random_fn fill, uint8_t *buf, uint32_t len);

/* poll_oneoff over native copies of the guest's arrays: waits until at least
 * one subscription fires, then writes one event per fired subscription. */
__wasi_errno_t WasiPollOneoff(vfs_ctx_t c, const __wasi_subscription_t *in,
                              __wasi_event_t *out, uint32_t n,
                              uint32_t *nevents);
