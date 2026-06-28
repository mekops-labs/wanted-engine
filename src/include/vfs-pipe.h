/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <vfs.h>

/* A pipe_store_t holds the named-pipe ring buffers plus the lock guarding them.
 * One store is shared by every wapp's /dev/pipe driver so pipes are visible
 * across the wapp boundary (inter-wapp IPC). The store is opaque to callers. */
typedef struct pipe_store_t pipe_store_t;

pipe_store_t *PipeStoreNew(void);
void PipeStoreFree(pipe_store_t *store);

/* Create a /dev/pipe driver backed by `store`. The per-wapp handle table lives
 * in the driver; the pipe storage lives in (and outlives) the shared store. */
vfs_driver_t *PipeDriverCreate(pipe_store_t *store);

/* Create a console driver bound to the single named pipe `name` in `store`.
 * `forRead` selects the `in` direction (a reader); otherwise it is a lossy
 * writer for `out`/`err` (drops oldest on a full ring so an unread console never
 * wedges the wapp). `flags` carries open flags (e.g. VFS_O_NONBLOCK). Installed
 * directly as a stream fd via VfsRegister — no Open call. */
vfs_driver_t *VfsPipeConsoleCreate(pipe_store_t *store, const char *name,
                                   bool forRead, vfs_oflags_t flags);
