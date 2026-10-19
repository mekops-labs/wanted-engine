/* SPDX-License-Identifier: Apache-2.0 */

/* The registry store that keeps everything in memory: the store of the dummy
 * platform and of the host tests. */

#ifndef REGISTRY_MEM_H
#define REGISTRY_MEM_H

#include <stddef.h>

#include <registry-store.h>

registry_store_t *RegistryMemStore(void);

/* Drops every blob and record and lifts the byte limit. */
void RegistryMemReset(void);

/* Refuses a stage that would take the blob bytes past `bytes`; 0 lifts the
 * limit. */
void RegistryMemSetLimit(size_t bytes);

#endif /* REGISTRY_MEM_H */
