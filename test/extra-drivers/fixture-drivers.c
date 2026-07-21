/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>

/* Two entries with one job each: `extra` proves the extra table is consulted
 * at all, and the `null` entry claims a core name so the resolver's precedence
 * can be asserted — a build that let it win would hand out this driver's
 * marker body instead of the core null device. */
#define EXTRA_BODY "EXTRA"
#define SHADOW_BODY "SHADOW"

struct vfs_driver_ctx_t {
    const char *body;
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);

static struct vfs_driver_ctx_t extraCtx = {EXTRA_BODY};
static struct vfs_driver_ctx_t shadowCtx = {SHADOW_BODY};

static vfs_driver_t extraDriver = {
    .id = {'X', 't', 'r', 'a'},
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .ctx = &extraCtx,
    .Destroy = _Destroy,
    .Open = _Open,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
};

static vfs_driver_t shadowDriver = {
    .id = {'S', 'h', 'a', 'd'},
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .ctx = &shadowCtx,
    .Destroy = _Destroy,
    .Open = _Open,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
};

static vfs_driver_t *extraInit(const wapp_t *wapp, const char *opt) {
    (void)wapp;
    (void)opt;
    return &extraDriver;
}

static vfs_driver_t *shadowInit(const wapp_t *wapp, const char *opt) {
    (void)wapp;
    (void)opt;
    return &shadowDriver;
}

static const vfs_driver_table_t extraTable[] = {
    {"extra", extraInit},
    {"null", shadowInit},
    {NULL, NULL},
};

const vfs_driver_table_t *ExtraDriverTable(void) { return extraTable; }

/* Signature fixed by the vfs_driver_t callback table. */
/* cppcheck-suppress constParameterCallback */
static int _Destroy(struct vfs_driver_t *d) {
    (void)d;
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)d;
    (void)path;
    (void)flags;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d;
    (void)fd;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)fd;
    memset(stat, 0, sizeof(*stat));
    stat->size = (uint32_t)strlen(d->body);
    return 0;
}

/* Signature fixed by the vfs_driver_t callback table. */
/* cppcheck-suppress constParameterCallback */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    size_t n = strlen(d->body);
    (void)fd;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, d->body, n);
    return (int)n;
}
