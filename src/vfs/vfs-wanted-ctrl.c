#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>

#include <vfs.h>
#include <vfs-drivers.h>

#include <platform.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>


#define ID  {'W', 'c', 't', 'l'}

static int _Destroy (struct vfs_driver_t *d);
static int _Open    (vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _OpenAt  (vfs_driver_ctx_t d, int fd, const char *path, vfs_oflags_t flags);
static int _Close   (vfs_driver_ctx_t d, int fd);
static int _Stat    (vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read    (vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write   (vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

const vfs_driver_t WantedControlDriver = {
    .id              = ID,
    .filetype        = VFS_FILETYPE_CHARACTER_DEVICE,
    .Destroy         = _Destroy,
    .Open            = _Open,
    .Close           = _Close,
    .Stat            = _Stat,
    .Read            = _Read,
    .Write           = _Write,
};

static bool opened = false;

static int _Destroy (struct vfs_driver_t *d)
{
    opened = false;

    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags)
{
    if (opened) return -EBUSY;
    opened = true;

    return 0;
}

static int _Close(vfs_driver_ctx_t d, int fd)
{
    opened = false;
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat)
{
    stat->dev = WantedControlDriver.bytesId;
    stat->ino = 0;
    stat->filetype = WantedControlDriver.filetype;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;

    return 0;
}
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte)
{
    if (buf == NULL) return -EINVAL;
    if (!opened) return -EBADF;

    static int read = 0;
    if (read > 0) {
        read = 0;
        return read;
    }

    read = WantedReadState(buf, nbyte);

    return read;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte)
{
    wapp_t wapp;
    wapp_action_t act;
    reg_entry_t e;
    int ret;
    const char *p = buf;

    if (buf == NULL) return -EINVAL;
    if (!opened) return -EBADF;

    ret = WantedParseCtrlAction(buf, nbyte, wapp.name, &act, &wapp.cfg);
    if (ret < 0) return ret;

    /* TODO: need to give more parameters, like what drivers need to be enabled. JSON? */
    switch (act) {
    case WAPP_START: {
        const char *ver = strchr(wapp.name, ':');
        if (ver != NULL) {
            ver += 1;
            size_t nameLen = ver - p > WAPP_MAX_NAME_LEN ? WAPP_MAX_NAME_LEN : ver - p;
            size_t verLen = strnlen(ver, WAPP_MAX_VERSION_LEN);
            strncpy(e.name, p, nameLen);
            e.name[nameLen-1] = '\0';

            strncpy(e.version, ver, verLen);
            e.version[verLen] = '\0';
        } else {
            strncpy(e.name, p, WAPP_MAX_NAME_LEN);
            e.version[0] = '\0';
        }

        ret = PlatformRegistryWappLoad(&e, &wapp);
        if (ret < 0) {
            return ret;
        }

        ret = WantedWappParseManifest(&wapp);
        if (ret < 0) {
            return ret;
        }

        ret = PlatformWappStart(wapp);
        if (ret < 0) {
            return ret;
        }
        break;
    }
    case WAPP_STOP: {
        ret = PlatformWappStop(wapp.name);
        if (ret < 0) {
            return ret;
        }
        break;
    }
    default:
        break;
    }

    return nbyte;
}
