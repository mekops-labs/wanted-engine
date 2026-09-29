/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>
#include <vfs.h>

#include "wasi_types.h"

/* A negative VFS errno as the WASI errno a guest sees. */
__wasi_errno_t WasiErrno(int errnum);

/* poll_oneoff over native copies of the guest's arrays: waits until at least
 * one subscription fires, then writes one event per fired subscription. */
__wasi_errno_t WasiPollOneoff(vfs_ctx_t c, const __wasi_subscription_t *in,
                              __wasi_event_t *out, uint32_t n,
                              uint32_t *nevents);
