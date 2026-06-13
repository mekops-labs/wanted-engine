/* SPDX-License-Identifier: Apache-2.0 */

/* "log" console driver — captures a wapp's stdout/stderr into the per-wapp log
 * store instead of the shared platform console. Installed as a wapp's console
 * (console.out/err = {"name":"log"}); the supervisor reads the captured output
 * back via /dev/wanted/wapps/<name>/log. A misbehaving or chatty wapp can no
 * longer disturb the platform console, and its late output is not lost to the
 * stdio teardown that closes the shared console fd.
 *
 * One driver instance per wapp (like vfs-config), bound to the wapp name; the
 * ring buffers live in the process-wide LogStore (vfs-pipe is the sibling
 * pattern). Writes append; reads return EOF (the log is read out-of-band by
 * the supervisor, not by the wapp itself). */

#include <string.h>

#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

#define ID                                                                     \
    { 'L', 'o', 'g', 's' }

struct vfs_driver_ctx_t {
    char name[WAPP_MAX_NAME_LEN];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

static const char id[4] = ID;

vfs_driver_t *VfsLogInit(const wapp_t *wapp, const char *opt) {
    (void)opt;
    if (wapp == NULL)
        return NULL;

    vfs_driver_t *driver = WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL)
        return NULL;
    memset(driver, 0, sizeof(*driver));

    driver->ctx = WantedMalloc(sizeof(struct vfs_driver_ctx_t));
    if (driver->ctx == NULL) {
        WantedFree(driver);
        return NULL;
    }
    strncpy(driver->ctx->name, wapp->name, WAPP_MAX_NAME_LEN - 1);
    driver->ctx->name[WAPP_MAX_NAME_LEN - 1] = '\0';

    driver->bytesId = *(const uint32_t *)id;
    driver->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->OpenAt = _OpenAt;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d) {
        WantedFree(d->ctx);
        WantedFree(d);
    }
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)d; (void)path; (void)flags;
    return 0;
}

static int _OpenAt(vfs_driver_ctx_t d, int fd, const char *path,
                   vfs_oflags_t flags) {
    (void)d; (void)fd; (void)path; (void)flags;
    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    (void)d; (void)fd;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)d; (void)fd;
    memset(stat, 0, sizeof(*stat));
    stat->dev = *(const uint32_t *)id;
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)d; (void)fd; (void)buf; (void)nbyte;
    return 0; /* EOF — the supervisor reads the log via the control plane */
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    (void)fd;
    if (d == NULL || buf == NULL)
        return -1;
    LogStoreAppend(LogStore(), d->name, buf, nbyte);
    return (int)nbyte;
}
