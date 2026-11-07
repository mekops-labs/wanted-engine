/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

/* A wbus_store_t holds the message pool, the endpoints, the pending calls and
 * the served objects, under one lock. Every wapp's /dev/wbus driver shares the
 * engine's store, so one wapp reaches another's objects. */
typedef struct wbus_store_t wbus_store_t;

/* Allocates the pool and the tables; NULL when memory runs out. */
wbus_store_t *WbusStoreNew(void);
void WbusStoreFree(wbus_store_t *store);

/* The engine-wide store every wapp's driver uses; created on first call. */
wbus_store_t *WbusStoreShared(void);

/* The /dev/wbus driver of wapp instance `wapp` over `store`, granted the
 * serve=, call= and listen= clauses of `grant`. NULL with errno set: EINVAL
 * (bad clause), ENOSPC (too many), EPERM (serve=supervisor, other instance). */
vfs_driver_t *WbusDriverCreate(wbus_store_t *store, const char *wapp,
                               const char *grant);
