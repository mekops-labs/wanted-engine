/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vfs.h>

typedef int (*proc_read_fn_t)(vfs_ctx_t c, void *buf, size_t bufLen);

int ProcFs_Register(vfs_ctx_t c, const char *name, proc_read_fn_t fn,
                    bool privileged);
void *ProcFs_Open(vfs_ctx_t c, const char *suffix, vfs_oflags_t flags,
                  int *out_err);
int ProcFs_Close(vfs_ctx_t c, void *handle);
int ProcFs_Read(vfs_ctx_t c, void *handle, void *buf, size_t nbyte);
int ProcFs_Stat(vfs_ctx_t c, void *handle, vfs_stat_t *stat);
int ProcFs_ReadDir(vfs_ctx_t c, void *handle, void *buf, size_t bufLen,
                   uint64_t *cookie, size_t *bufUsed);
void ProcFs_Destroy(vfs_ctx_t c);
