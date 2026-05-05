#include <errno.h>
#include <stddef.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <vfs-stdio.h>
#include <vfs.h>
#include <wanted_malloc.h>

/* Stdio DevFS drivers — /dev/stdin, /dev/stdout, /dev/stderr.
 *
 * Phase 4 stub: devices are registered so they appear in ls /dev.
 * Reads on stdin return 0 (EOF). Writes on stdout/stderr return nbyte.
 * Phase 4+ will replace these with vfs_stdio_backend_t-backed drivers. */

static int _stdio_Open(vfs_driver_ctx_t ctx, const char *path,
                       vfs_oflags_t flags) {
    (void)ctx; (void)path; (void)flags;
    return 0;
}

static int _stdio_Close(vfs_driver_ctx_t ctx, int fd) {
    (void)ctx; (void)fd;
    return 0;
}

static int _stdio_Stat(vfs_driver_ctx_t ctx, int fd, vfs_stat_t *stat) {
    (void)ctx; (void)fd;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _stdin_Read(vfs_driver_ctx_t ctx, int fd, void *buf, size_t nbyte) {
    (void)ctx; (void)fd; (void)buf; (void)nbyte;
    return 0; /* EOF */
}

static int _stdout_Write(vfs_driver_ctx_t ctx, int fd, const void *buf,
                         size_t nbyte) {
    (void)ctx; (void)fd; (void)buf;
    return (int)nbyte;
}

static vfs_driver_t StdinDriver = {
    .id       = {'s','t','i','n'},
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Open     = _stdio_Open,
    .Close    = _stdio_Close,
    .Stat     = _stdio_Stat,
    .Read     = _stdin_Read,
};

static vfs_driver_t StdoutDriver = {
    .id       = {'s','t','o','u'},
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Open     = _stdio_Open,
    .Close    = _stdio_Close,
    .Stat     = _stdio_Stat,
    .Write    = _stdout_Write,
};

static vfs_driver_t StderrDriver = {
    .id       = {'s','t','e','r'},
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Open     = _stdio_Open,
    .Close    = _stdio_Close,
    .Stat     = _stdio_Stat,
    .Write    = _stdout_Write,
};

vfs_driver_t *VfsStdinDriverGet(void)  { return &StdinDriver; }
vfs_driver_t *VfsStdoutDriverGet(void) { return &StdoutDriver; }
vfs_driver_t *VfsStderrDriverGet(void) { return &StderrDriver; }
