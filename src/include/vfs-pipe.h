#pragma once

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
