/* SPDX-License-Identifier: Apache-2.0 */

#include <registry-fs.h>

size_t PlatformRegistryStores(registry_store_t **out, size_t max) {
    if (max == 0)
        return 0;
    out[0] = RegistryFsStore();
    return 1;
}
