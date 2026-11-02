/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <vfs.h>

/* Longest pipe name; names are [A-Za-z0-9._-]. */
#define PIPE_NAME_MAX 31

/* A pipe_store_t holds the named-pipe ring buffers plus the lock guarding them.
 * One store is shared by every wapp's /dev/pipe driver so pipes are visible
 * across the wapp boundary (inter-wapp IPC). The store is opaque to callers. */
typedef struct pipe_store_t pipe_store_t;

pipe_store_t *PipeStoreNew(void);
void PipeStoreFree(pipe_store_t *store);

/* The engine-wide store every wapp's driver uses; created on first call. */
pipe_store_t *PipeStoreShared(void);

/* 0 for a valid name; -ENAMETOOLONG past PIPE_NAME_MAX, -EINVAL otherwise. */
int PipeNameCheck(const char *name);

/* Create a /dev/pipe driver over `store` that reaches every pipe. The pipe
 * storage outlives the driver. */
vfs_driver_t *PipeDriverCreate(pipe_store_t *store);

/* As PipeDriverCreate, reaching only the read=/write= names `grant` lists.
 * NULL or "" lists none. Returns NULL on a malformed grant. */
vfs_driver_t *PipeDriverCreateScoped(pipe_store_t *store, const char *grant);

/* Add `grant`'s clauses to a scoped driver, all or none. -ENOSPC when it would
 * list too many names; -EINVAL or -ENAMETOOLONG for a bad clause or name. */
int PipeDriverGrant(vfs_driver_t *drv, const char *grant);

/* Parse a pipe console's name=<pipe> and eof options; `name` is "" when none
 * is pinned. -ENAMETOOLONG or -EINVAL for a bad name or option. */
int PipeConsoleOptions(const char *options, char *name, size_t cap, bool *eof);

/* Console driver on pipe `name`; `forRead` selects `in`. With `eof` the stream
 * ends when the console is destroyed: readers see end of stream, writers to
 * `in` get -EPIPE. NULL when `name` fails PipeNameCheck. */
vfs_driver_t *VfsPipeConsoleCreate(pipe_store_t *store, const char *name,
                                   bool forRead, vfs_oflags_t flags, bool eof);
