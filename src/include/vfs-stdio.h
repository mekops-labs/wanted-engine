/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <vfs.h>

/* Build a /dev/std{in,out,err} alias driver that forwards to a console stream
 * slot's driver (`target`) using that slot's driver-fd (`target_fd`, i.e.
 * 0/1/2) — so the /dev path mirrors the wapp's WASI fd. The alias does not own
 * `target`; passing NULL yields a benign stub (reads EOF, writes discard). */
vfs_driver_t *VfsStdioAliasInit(const vfs_driver_t *target, int target_fd);
