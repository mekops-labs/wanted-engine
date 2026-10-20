/* SPDX-License-Identifier: Apache-2.0 */

/* The registry store on a POSIX filesystem: one file for each blob, named by
 * its digest, and one file for each entry record. */

#ifndef REGISTRY_FS_H
#define REGISTRY_FS_H

#include <registry-store.h>

registry_store_t *RegistryFsStore(void);

#endif /* REGISTRY_FS_H */
