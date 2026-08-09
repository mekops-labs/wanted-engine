/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <vfs.h>

/* Build a /dev/std{in,out,err} alias driver forwarding to a console stream
 * slot's driver (`target`) using that slot's driver-fd (0/1/2). The alias does
 * not own `target`; NULL yields a benign stub. */
vfs_driver_t *VfsStdioAliasInit(const vfs_driver_t *target, int target_fd);
