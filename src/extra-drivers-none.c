/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include <vfs-drivers.h>

/* Default extra-driver table for a build with no out-of-tree driver tree
 * (WANTED_EXTRA_DRIVERS_DIR, or an ESP-IDF board directory's sources), which
 * compiles its own definition in place of this one. */
const vfs_driver_table_t *ExtraDriverTable(void) { return NULL; }
