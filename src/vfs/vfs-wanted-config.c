#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <vfs-drivers.h>
#include <vfs.h>

#include <wanted-vfs-api.h>

/* ── WantedConfigDriver ─────────────────────────────────────────────────────
 *
 * Returns a meta-config that tells the supervisor where to find the wapp
 * list.  The supervisor reads this file first, extracts config_file, then
 * opens that path to get the actual wapp array.
 */

#define CONFIG_ID                                                              \
    { 'W', 'c', 'f', 'g' }

static const char WANTED_META_CFG[] =
    "{\"config_file\":\"/dev/wanted/wapps\",\"debug\":false}";

static int _cfg_Destroy(struct vfs_driver_t *d);
static int _cfg_Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _cfg_Close(vfs_driver_ctx_t d, int fd);
static int _cfg_Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _cfg_Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _cfg_Write(vfs_driver_ctx_t d, int fd, const void *buf,
                      size_t nbyte);

const vfs_driver_t WantedConfigDriver = {
    .id = CONFIG_ID,
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Destroy = _cfg_Destroy,
    .Open = _cfg_Open,
    .Close = _cfg_Close,
    .Stat = _cfg_Stat,
    .Read = _cfg_Read,
    .Write = _cfg_Write,
};

static bool cfg_opened = false;

static int _cfg_Destroy(struct vfs_driver_t *d) {
    cfg_opened = false;
    return 0;
}

static int _cfg_Open(vfs_driver_ctx_t d, const char *path,
                     vfs_oflags_t flags) {
    if (cfg_opened)
        return -EBUSY;
    cfg_opened = true;
    return 0;
}

static int _cfg_Close(vfs_driver_ctx_t d, int fd) {
    cfg_opened = false;
    return 0;
}

static int _cfg_Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    stat->dev = WantedConfigDriver.bytesId;
    stat->ino = 0;
    stat->filetype = WantedConfigDriver.filetype;
    stat->nlink = 0;
    stat->size = sizeof(WANTED_META_CFG) - 1;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;
    return 0;
}

static int _cfg_Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (!cfg_opened)
        return -EBADF;

    static int cfg_read = 0;
    if (cfg_read > 0) {
        cfg_read = 0;
        return cfg_read;
    }

    size_t n = sizeof(WANTED_META_CFG) - 1;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, WANTED_META_CFG, n);
    cfg_read = (int)n;
    return cfg_read;
}

static int _cfg_Write(vfs_driver_ctx_t d, int fd, const void *buf,
                      size_t nbyte) {
    return -EROFS;
}

/* ── WantedWappsDriver ──────────────────────────────────────────────────────
 *
 * Returns the wapp list as a JSON object array so the supervisor's
 * parseWappsConfig can consume it: {"wapps":[{"name":"a"},...]}
 */

#define WAPPS_ID                                                               \
    { 'W', 'a', 'p', 's' }

static int _wapps_Destroy(struct vfs_driver_t *d);
static int _wapps_Open(vfs_driver_ctx_t d, const char *path,
                       vfs_oflags_t flags);
static int _wapps_Close(vfs_driver_ctx_t d, int fd);
static int _wapps_Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _wapps_Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _wapps_Write(vfs_driver_ctx_t d, int fd, const void *buf,
                        size_t nbyte);

const vfs_driver_t WantedWappsDriver = {
    .id = WAPPS_ID,
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Destroy = _wapps_Destroy,
    .Open = _wapps_Open,
    .Close = _wapps_Close,
    .Stat = _wapps_Stat,
    .Read = _wapps_Read,
    .Write = _wapps_Write,
};

static bool wapps_opened = false;

static int _wapps_Destroy(struct vfs_driver_t *d) {
    wapps_opened = false;
    return 0;
}

static int _wapps_Open(vfs_driver_ctx_t d, const char *path,
                       vfs_oflags_t flags) {
    if (wapps_opened)
        return -EBUSY;
    wapps_opened = true;
    return 0;
}

static int _wapps_Close(vfs_driver_ctx_t d, int fd) {
    wapps_opened = false;
    return 0;
}

static int _wapps_Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    stat->dev = WantedWappsDriver.bytesId;
    stat->ino = 0;
    stat->filetype = WantedWappsDriver.filetype;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;
    return 0;
}

static int _wapps_Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (!wapps_opened)
        return -EBADF;

    static int wapps_read = 0;
    if (wapps_read > 0) {
        wapps_read = 0;
        return wapps_read;
    }

    wapps_read = WantedGetConfigJson(buf, nbyte);
    return wapps_read;
}

static int _wapps_Write(vfs_driver_ctx_t d, int fd, const void *buf,
                        size_t nbyte) {
    return -EROFS;
}
