#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

#define MAX_FILE_SIZE 4096

static const char id[] = {'C', 'o', 'n', 'f'};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);

struct vfs_driver_ctx_t {
    bool opened;
    const char *contents;
    size_t len;
    size_t offset;
};

vfs_driver_t *VfsConfigInit(const wapp_t *wapp, const char *opt) {
    vfs_driver_t *driver;

    if (NULL == wapp || NULL == opt) {
        DEBUG_TRACE("bad arguments");
        return NULL;
    }

    driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    driver->ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == driver->ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }

    driver->ctx->len = strnlen(opt, MAX_FILE_SIZE);
    driver->ctx->contents = (char *)WantedMalloc(driver->ctx->len);
    if (NULL == driver->ctx->contents) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver->ctx);
        WantedFree(driver);
        return NULL;
    }

    if (driver->ctx->len > 0) {
        memcpy((char *)driver->ctx->contents, opt, driver->ctx->len);
    }

    driver->bytesId = *(uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_REGULAR_FILE;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Seek = _Seek;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    d->ctx->opened = false;

    WantedFree((void *)d->ctx->contents);
    WantedFree(d->ctx);
    WantedFree(d);

    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    // TODO: fix for esp and linux platforms
    // if (d->opened) return -EBUSY;
    if ((flags & 0x3) != VFS_O_RDONLY)
        return -EROFS;

    d->opened = true;
    d->offset = 0;

    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    d->opened = false;

    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    if (stat == NULL)
        return -EINVAL;

    stat->dev = *(uint32_t *)id;
    stat->ino = 0;
    stat->filetype = VFS_FILETYPE_REGULAR_FILE;
    stat->nlink = 0;
    stat->size = d->len;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (!d->opened)
        return -EBADF;

    int read = d->offset + nbyte > d->len ? d->len - d->offset : nbyte;
    if (read == 0)
        return 0;

    memcpy(buf, d->contents + d->offset, read);
    d->offset += read;

    return read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (!d->opened)
        return -EBADF;
    return -EROFS;
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos) {
    DEBUG_TRACE("%d, %ld (%u)", fd, off, whence);
    if (!d->opened)
        return -EBADF;

    switch (whence) {
    case VFS_SEEK_SET:
        d->offset = off > d->len ? d->len : off;
        break;
    case VFS_SEEK_CUR:
        if (off + d->offset > d->len)
            off = d->len - d->offset;
        d->offset += off;
        break;
    case VFS_SEEK_END:
        if (off > d->len)
            off = d->len;
        d->offset = d->len - off;
        break;
    default:
        return -EINVAL;
    }

    *pos = d->offset;

    return 0;
}
