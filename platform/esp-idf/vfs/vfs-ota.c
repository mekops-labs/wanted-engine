/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF OTA VFS driver, exposed to a wapp as a text control node plus a
 * binary image sink. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted_malloc.h>

static const char id[] = {'O', 't', 'a', '\0'};

#define OTA_MAX_FDS 2      /* concurrent opens of either node */
#define OTA_CMD_MAX 16     /* longest accepted command line ("rollback\n") */
#define OTA_STATUS_MAX 128 /* status text block */

enum ota_node_t {
    OTA_NODE_CTL,  /* /dev/ota */
    OTA_NODE_SLOT, /* /dev/ota/slot */
};

struct ota_fd_t {
    bool used;
    enum ota_node_t node;
    char *status; /* CTL only: heap status snapshot, drained by reads */
    size_t status_len;
    size_t status_off;
};

struct vfs_driver_ctx_t {
    struct ota_fd_t fds[OTA_MAX_FDS];
};

static char slotOrNone(char c, char buf[2]) {
    if (c == '\0')
        return '\0';
    buf[0] = c;
    buf[1] = '\0';
    return c;
}

/* Builds one status snapshot, heap-owned by the caller. NULL on failure or
 * if the platform layer has no boot state to report. */
static char *buildStatus(void) {
    platform_ota_state_t st;
    if (PlatformOtaGetBootState(&st) != 0)
        return NULL;

    char pendingBuf[2], failedBuf[2];
    const char *pendingSlot = "none";
    const char *lastFailed = "none";

    if (st.pending_swap) {
        /* Exactly two OTA slots -- the pending one is always the other. */
        char other = (st.active_slot == 'a') ? 'b' : 'a';
        slotOrNone(other, pendingBuf);
        pendingSlot = pendingBuf;
    }
    if (st.last_failed_slot != '\0') {
        slotOrNone(st.last_failed_slot, failedBuf);
        lastFailed = failedBuf;
    }

    char *out = (char *)WantedMalloc(OTA_STATUS_MAX);
    if (out == NULL)
        return NULL;

    int n = snprintf(out, OTA_STATUS_MAX,
                     "active_slot: %c\n"
                     "status: %s\n"
                     "pending_slot: %s\n"
                     "last_failed_slot: %s\n"
                     "boot_attempts: %d\n",
                     st.active_slot, st.confirmed ? "confirmed" : "unconfirmed",
                     pendingSlot, lastFailed, st.boot_attempts);
    if (n < 0) {
        WantedFree(out);
        return NULL;
    }
    return out;
}

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsOtaInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;

    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL)
        return NULL;

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (ctx == NULL) {
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    for (int i = 0; i < OTA_MAX_FDS; i++)
        WantedFree(ctx->fds[i].status);
    WantedFree(ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    enum ota_node_t node;
    if (path == NULL || path[0] == '\0')
        node = OTA_NODE_CTL;
    else if (strcmp(path, "slot") == 0)
        node = OTA_NODE_SLOT;
    else
        return -ENOENT;

    for (int i = 0; i < OTA_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= OTA_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    WantedFree(d->fds[fd].status);
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    (void)d;
    (void)fd;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    s->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* /dev/ota: stream a fresh status snapshot per drain cycle. /dev/ota/slot is
 * write-only. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= OTA_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct ota_fd_t *f = &d->fds[fd];
    if (f->node == OTA_NODE_SLOT)
        return -EPERM;

    if (f->status == NULL) {
        f->status = buildStatus();
        if (f->status == NULL)
            return -EIO;
        f->status_len = strlen(f->status);
        f->status_off = 0;
    }

    size_t left = f->status_len - f->status_off;
    if (left == 0) {
        WantedFree(f->status);
        f->status = NULL;
        f->status_len = f->status_off = 0;
        return 0; /* EOF for this snapshot; next read starts a fresh one */
    }

    size_t n = (nbyte < left) ? nbyte : left;
    memcpy(buf, f->status + f->status_off, n);
    f->status_off += n;
    return (int)n;
}

/* /dev/ota: one command per call ("begin"/"commit"/"confirm"/"rollback").
 * /dev/ota/slot: raw image bytes, forwarded to PlatformOtaWrite. */
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= OTA_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    const struct ota_fd_t *f = &d->fds[fd];

    if (f->node == OTA_NODE_SLOT) {
        int rc = PlatformOtaWrite((const uint8_t *)buf, nbyte);
        return (rc < 0) ? rc : (int)nbyte;
    }

    if (nbyte == 0)
        return 0;

    char cmd[OTA_CMD_MAX];
    size_t len = (nbyte < sizeof(cmd) - 1) ? nbyte : sizeof(cmd) - 1;
    memcpy(cmd, buf, len);
    cmd[len] = '\0';
    if (len > 0 && cmd[len - 1] == '\n')
        cmd[len - 1] = '\0';

    int rc;
    if (strcmp(cmd, "begin") == 0)
        rc = PlatformOtaBeginWrite();
    else if (strcmp(cmd, "commit") == 0)
        rc = PlatformOtaCommit();
    else if (strcmp(cmd, "confirm") == 0)
        rc = PlatformOtaConfirm();
    else if (strcmp(cmd, "rollback") == 0)
        rc = PlatformOtaRollback();
    else
        return -EINVAL;

    return (rc < 0) ? rc : (int)nbyte;
}
