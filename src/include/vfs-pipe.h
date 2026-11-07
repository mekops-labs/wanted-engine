/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

/* Create a /dev/pipe driver over `store` that reaches every pipe, for tests.
 * The engine gives every wapp a scoped driver. The storage outlives it. */
vfs_driver_t *PipeDriverCreate(pipe_store_t *store);

/* As PipeDriverCreate, reaching only the read=/write= names `grant` lists.
 * NULL or "" lists none. Returns NULL on a malformed grant. */
vfs_driver_t *PipeDriverCreateScoped(pipe_store_t *store, const char *grant);

/* Add `grant`'s clauses to a scoped driver, all or none. -ENOSPC when it would
 * list too many names; -EINVAL or -ENAMETOOLONG for a bad clause or name. */
int PipeDriverGrant(vfs_driver_t *drv, const char *grant);

/* Name `drv` as the pipe driver of the running wapp `wapp`, so the supervisor
 * can grant it runtime clauses. The binding ends with the driver. -EINVAL for
 * a driver that is not a pipe driver, -ENOSPC when the table is full. */
int PipeDriverBind(vfs_driver_t *drv, const char *wapp);

/* Add (or, with `revoke`, remove) a running wapp's runtime clauses, all or
 * none. Errors: -ENOSPC full, -EINVAL bad clause, -ENOENT wapp not running or
 * clause not held, -EPERM a revoked clause only the launch grant gives. */
int PipeRuntimeGrant(const char *wapp, bool revoke, const char *clauses);

/* Write the runtime clauses of a running wapp, one "read=<name>" or
 * "write=<name>" per line. The length, or -ENOENT when it is not running. */
int PipeRuntimeList(const char *wapp, char *buf, size_t cap);

/* When a console pipe's stream ends with its wapp: never, on any end (`eof`),
 * or only on a clean exit with code 0 (`eof=ok`). */
typedef enum {
    PIPE_EOF_NONE,
    PIPE_EOF_ANY,
    PIPE_EOF_OK,
} pipe_eof_t;

/* Parse a pipe console's name=<pipe> and eof options; `name` is "" when none
 * is pinned. -ENAMETOOLONG or -EINVAL for a bad name or option. */
int PipeConsoleOptions(const char *options, char *name, size_t cap,
                       pipe_eof_t *eof);

/* Console driver on pipe `name`; `forRead` selects `in`. When `eof` matches how
 * the wapp ended (`*exitCode`, NULL is unclean), the stream ends. NULL on a bad
 * name, a full store or no memory. */
vfs_driver_t *VfsPipeConsoleCreate(pipe_store_t *store, const char *name,
                                   bool forRead, vfs_oflags_t flags,
                                   pipe_eof_t eof, const int32_t *exitCode);
