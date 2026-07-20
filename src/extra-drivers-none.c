/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include <vfs-drivers.h>

/* Default extra-driver table for builds configured without
 * WANTED_EXTRA_DRIVERS_DIR. Setting the option compiles the out-of-tree tree's
 * definition in place of this one. */
const vfs_driver_table_t *ExtraDriverTable(void) { return NULL; }
