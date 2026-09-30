/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs-input.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* Input device driver: /dev/input/<device>/{events,info}, one subtree per
 * granted device. Devices live in an engine-wide table so a backing or an
 * injector, which are not the owning wapp, reach the same queue. */

static const char id[] = {'I', 'n', 'D', 'v'};

#define INPUT_MAX_FDS 8
#define INPUT_DEVICES_KEY "devices="
#define INPUT_KEYMAP_KEY "keymap="
#define INPUT_INJECT_TOKEN "inject"
#define INPUT_SLEEP_NS 1000000ULL /* 1 ms */

#define INPUT_NODE_ROOT 0
#define INPUT_NODE_DEVICE 1
#define INPUT_NODE_EVENTS 2
#define INPUT_NODE_INFO 3
#define INPUT_NODE_INJECT 4

#define INPUT_INFO_MAX 48
#define INPUT_BYTE_BITS 8
#define INPUT_BYTE_MASK 0xFFU
#define INPUT_TYPE_ALL (INPUT_TYPE_KEY | INPUT_TYPE_TEXT | INPUT_TYPE_REL)

struct input_device_t {
    char name[INPUT_NAME_MAX + 1];
    char keymap[INPUT_NAME_MAX + 1];
    uint8_t types;
    bool owned;
    wanted_input_event_t q[INPUT_QUEUE_LEN];
    uint8_t head;
    uint8_t count;
};

static struct {
    input_device_t devices[CONFIG_WANTED_INPUT_MAX_DEVICES];
    uint8_t count;
    platform_mutex_t *lock;
} table;

struct input_fd_t {
    bool used;
    uint8_t node;
    uint8_t device;
    bool nonblock;
    uint32_t offset;
};

struct vfs_driver_ctx_t {
    input_device_t *devices[CONFIG_WANTED_INPUT_MAX_DEVICES];
    uint8_t count;
    bool inject; /* write-side grant: no events node, no ownership */
    bool claimed;
    int wakeFd;
    struct input_fd_t fds[INPUT_MAX_FDS];
};

/* ── Device table ────────────────────────────────────────────────────────── */

/* [A-Za-z0-9_-], 1..INPUT_NAME_MAX characters. */
static bool validToken(const char *s) {
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
            return false;
    }
    return n > 0 && n <= INPUT_NAME_MAX;
}

static input_device_t *findDevice(const char *name, size_t nameLen) {
    for (uint8_t i = 0; i < table.count; i++) {
        if (strlen(table.devices[i].name) == nameLen &&
            strncmp(table.devices[i].name, name, nameLen) == 0)
            return &table.devices[i];
    }
    return NULL;
}

int InputDeviceRegister(const input_device_desc_t *desc, input_device_t **out) {
    if (desc == NULL || desc->name == NULL || desc->keymap == NULL ||
        !validToken(desc->name) || !validToken(desc->keymap) ||
        desc->types == 0 || (desc->types & ~INPUT_TYPE_ALL) != 0)
        return -EINVAL;

    input_device_t *dev = findDevice(desc->name, strlen(desc->name));
    if (dev != NULL) {
        bool same =
            dev->types == desc->types && strcmp(dev->keymap, desc->keymap) == 0;
        if (!same)
            return -EEXIST;
    } else {
        if (table.count >= CONFIG_WANTED_INPUT_MAX_DEVICES)
            return -ENOSPC;
        dev = &table.devices[table.count];
        memset(dev, 0, sizeof(*dev));
        strncpy(dev->name, desc->name, INPUT_NAME_MAX);
        strncpy(dev->keymap, desc->keymap, INPUT_NAME_MAX);
        dev->types = desc->types;
        if (table.lock == NULL)
            table.lock = PlatformMutexNew();
        table.count++;
    }
    if (out != NULL)
        *out = dev;
    return 0;
}

void InputDevicesReset(void) {
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

void InputDevicePush(input_device_t *dev, const wanted_input_event_t *ev) {
    if (dev == NULL || ev == NULL)
        return;

    PlatformMutexLock(table.lock);
    if (dev->count >= INPUT_QUEUE_LEN) {
        dev->head = 0;
        dev->count = 1;
        dev->q[0] = (wanted_input_event_t){1, EV_SYN, SYN_DROPPED, 0};
    } else {
        dev->q[(dev->head + dev->count) % INPUT_QUEUE_LEN] = *ev;
        dev->count++;
    }
    PlatformMutexUnlock(table.lock);
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse devices=<name>[,<name>...],keymap=<keymap>[,inject] and resolve every
 * name. The keymap must be the one each device declares. Returns 0 or -EINVAL;
 * the caller frees the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t devKeyLen = sizeof(INPUT_DEVICES_KEY) - 1;
    const size_t mapKeyLen = sizeof(INPUT_KEYMAP_KEY) - 1;
    const char *keymap = NULL;
    size_t keymapLen = 0;

    if (options == NULL ||
        strncmp(options, INPUT_DEVICES_KEY, devKeyLen) != 0) {
        DEBUG_TRACE("input grant has no devices= clause");
        return -EINVAL;
    }

    const char *p = options + devKeyLen;
    for (;;) {
        const char *end = p;
        while (*end != '\0' && *end != ',')
            end++;
        size_t len = (size_t)(end - p);

        if (len == 0 || keymap != NULL)
            return -EINVAL;
        if (len >= mapKeyLen && strncmp(p, INPUT_KEYMAP_KEY, mapKeyLen) == 0) {
            if (len == mapKeyLen)
                return -EINVAL;
            keymap = p + mapKeyLen;
            keymapLen = len - mapKeyLen;
            if (*end == ',') {
                if (strcmp(end + 1, INPUT_INJECT_TOKEN) != 0)
                    return -EINVAL;
#ifdef CONFIG_WANTED_VFS_INPUT_INJECT
                ctx->inject = true;
#else
                return -EINVAL;
#endif
                break;
            }
        } else {
            input_device_t *d = findDevice(p, len);
            if (d == NULL || ctx->count >= CONFIG_WANTED_INPUT_MAX_DEVICES) {
                DEBUG_TRACE("input grant names an unknown device");
                return -EINVAL;
            }
            for (uint8_t i = 0; i < ctx->count; i++) {
                if (ctx->devices[i] == d)
                    return -EINVAL;
            }
            ctx->devices[ctx->count++] = d;
        }

        if (*end == '\0')
            break;
        p = end + 1;
    }

    if (keymap == NULL || ctx->count == 0)
        return -EINVAL;
    for (uint8_t i = 0; i < ctx->count; i++) {
        const char *have = ctx->devices[i]->keymap;
        if (strlen(have) != keymapLen ||
            strncmp(have, keymap, keymapLen) != 0) {
            DEBUG_TRACE("input grant names an unknown keymap");
            return -EINVAL;
        }
    }
    return 0;
}

/* Take every granted device for this owner, or none of them. Records queued
 * before the claim are dropped. */
static int claimDevices(struct vfs_driver_ctx_t *ctx) {
    int rc = 0;

    if (ctx->inject)
        return 0;

    PlatformMutexLock(table.lock);
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (ctx->devices[i]->owned)
            rc = -EBUSY;
    }
    if (rc == 0) {
        for (uint8_t i = 0; i < ctx->count; i++) {
            ctx->devices[i]->owned = true;
            ctx->devices[i]->head = 0;
            ctx->devices[i]->count = 0;
        }
        ctx->claimed = true;
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

static void releaseDevices(struct vfs_driver_ctx_t *ctx) {
    if (!ctx->claimed)
        return;
    PlatformMutexLock(table.lock);
    for (uint8_t i = 0; i < ctx->count; i++)
        ctx->devices[i]->owned = false;
    PlatformMutexUnlock(table.lock);
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *device) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = INPUT_NODE_ROOT;
        *device = 0;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;

    int idx = -1;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strlen(ctx->devices[i]->name) == (size_t)(slash - p) &&
            strncmp(ctx->devices[i]->name, p, (size_t)(slash - p)) == 0)
            idx = i;
    }
    if (idx < 0)
        return -ENOENT;
    *device = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0')
        *node = INPUT_NODE_DEVICE;
    else if (strcmp(slash, "events") == 0 && !ctx->inject)
        *node = INPUT_NODE_EVENTS;
    else if (strcmp(slash, "info") == 0)
        *node = INPUT_NODE_INFO;
    else if (strcmp(slash, INPUT_INJECT_TOKEN) == 0 && ctx->inject)
        *node = INPUT_NODE_INJECT;
    else
        return -ENOENT;
    return 0;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 long *pos);
static void _SetWake(vfs_driver_ctx_t d, int fd);
static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags);
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsInputInit(const wapp_t *wapp, const char *options) {
    (void)wapp;

    vfs_driver_t *driver = WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }
    struct vfs_driver_ctx_t *ctx = WantedMalloc(sizeof(*ctx));
    if (ctx == NULL) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->wakeFd = -1;
    memset(driver, 0, sizeof(*driver));

    driver->bytesId = *(const uint32_t *)(id);
    driver->filetype = VFS_FILETYPE_DIRECTORY;
    driver->ctx = ctx;
    driver->Destroy = _Destroy;
    driver->Open = _Open;
    driver->Close = _Close;
    driver->Stat = _Stat;
    driver->Read = _Read;
    driver->Write = _Write;
    driver->Seek = _Seek;
    driver->SetWake = _SetWake;
    driver->SetFlags = _SetFlags;
    driver->Poll = _Poll;
    driver->ReadDir = _ReadDir;

    if (parseGrant(ctx, options) < 0 || claimDevices(ctx) < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    if (d->ctx != NULL)
        releaseDevices(d->ctx);
    WantedFree(d->ctx);
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct input_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= INPUT_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    uint8_t device;
    int rc = resolve(d, path != NULL ? path : "", &node, &device);
    if (rc < 0)
        return rc;

    for (int i = 0; i < INPUT_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            d->fds[i].device = device;
            d->fds[i].nonblock = (flags & VFS_O_NONBLOCK) != 0;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == INPUT_NODE_ROOT || f->node == INPUT_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static void _SetWake(vfs_driver_ctx_t d, int fd) { d->wakeFd = fd; }

static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags) {
    struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    f->nonblock = (flags & VFS_O_NONBLOCK) != 0;
    return 0;
}

/* Only events waits on anything; every other node is always ready. */
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail) {
    const struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == INPUT_NODE_EVENTS) {
        PlatformMutexLock(table.lock);
        uint8_t count = d->devices[f->device]->count;
        PlatformMutexUnlock(table.lock);
        *avail = (uint32_t)count * INPUT_EVENT_BYTES;
        return count > 0 ? VFS_POLL_IN : 0;
    }
    return VFS_POLL_IN | VFS_POLL_OUT;
}

/* "<types> keymap=<keymap>\n", types in the order key, text, rel. */
static int readInfo(vfs_driver_ctx_t d, struct input_fd_t *f, void *buf,
                    size_t nbyte) {
    const input_device_t *dev = d->devices[f->device];
    char line[INPUT_INFO_MAX];
    size_t len = 0;

    if (dev->types & INPUT_TYPE_KEY)
        len += (size_t)snprintf(line + len, sizeof(line) - len, "key ");
    if (dev->types & INPUT_TYPE_TEXT)
        len += (size_t)snprintf(line + len, sizeof(line) - len, "text ");
    if (dev->types & INPUT_TYPE_REL)
        len += (size_t)snprintf(line + len, sizeof(line) - len, "rel ");
    len += (size_t)snprintf(line + len, sizeof(line) - len, "keymap=%s\n",
                            dev->keymap);
    if (len >= sizeof(line) || f->offset >= len)
        return 0;

    size_t n = len - f->offset;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, line + f->offset, n);
    f->offset += (uint32_t)n;
    return (int)n;
}

static void putRecord(uint8_t *p, const wanted_input_event_t *ev) {
    p[0] = ev->sync;
    p[1] = ev->type;
    p[2] = (uint8_t)(ev->code & INPUT_BYTE_MASK);
    p[3] = (uint8_t)(ev->code >> INPUT_BYTE_BITS);
    uint32_t v = (uint32_t)ev->value;
    for (size_t i = 0; i < sizeof(v); i++)
        p[4 + i] = (uint8_t)((v >> (INPUT_BYTE_BITS * i)) & INPUT_BYTE_MASK);
}

/* Pop whole records into `buf`. Returns bytes copied, 0 when none queued. */
static int popEvents(input_device_t *dev, uint8_t *buf, size_t nbyte) {
    int used = 0;

    PlatformMutexLock(table.lock);
    while (dev->count > 0 && (size_t)used + INPUT_EVENT_BYTES <= nbyte) {
        putRecord(buf + used, &dev->q[dev->head]);
        dev->head = (uint8_t)((dev->head + 1) % INPUT_QUEUE_LEN);
        dev->count--;
        used += INPUT_EVENT_BYTES;
    }
    PlatformMutexUnlock(table.lock);
    return used;
}

/* Blocks until a record is queued; a stop ends the wait. */
static int readEvents(vfs_driver_ctx_t d, struct input_fd_t *f, void *buf,
                      size_t nbyte) {
    if (nbyte < INPUT_EVENT_BYTES)
        return -EINVAL;

    for (;;) {
        int n = popEvents(d->devices[f->device], buf, nbyte);
        if (n > 0)
            return n;
        if (f->nonblock)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, INPUT_SLEEP_NS, 0) ==
                -EINTR ||
            PlatformWakeRaised(d->wakeFd))
            return -EINTR;
    }
}

#ifdef CONFIG_WANTED_VFS_INPUT_INJECT
static void getRecord(const uint8_t *p, wanted_input_event_t *ev) {
    uint32_t v = 0;
    for (size_t i = 0; i < sizeof(v); i++)
        v |= (uint32_t)p[4 + i] << (INPUT_BYTE_BITS * i);
    ev->sync = p[0];
    ev->type = p[1];
    ev->code = (uint16_t)(p[2] | (p[3] << INPUT_BYTE_BITS));
    ev->value = (int32_t)v;
}

/* Queue whole records for the device's owner; a partial record queues none. */
static int writeInject(vfs_driver_ctx_t d, const struct input_fd_t *f,
                       const void *buf, size_t nbyte) {
    if (nbyte % INPUT_EVENT_BYTES != 0)
        return -EINVAL;

    for (size_t off = 0; off < nbyte; off += INPUT_EVENT_BYTES) {
        wanted_input_event_t ev;
        getRecord((const uint8_t *)buf + off, &ev);
        InputDevicePush(d->devices[f->device], &ev);
    }
    return (int)nbyte;
}
#endif

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case INPUT_NODE_INFO:
        return readInfo(d, f, buf, nbyte);
    case INPUT_NODE_EVENTS:
        return readEvents(d, f, buf, nbyte);
    case INPUT_NODE_ROOT:
    case INPUT_NODE_DEVICE:
        return -EISDIR;
    case INPUT_NODE_INJECT:
        return -EPERM;
    default:
        return -ENOSYS;
    }
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    switch (f->node) {
    case INPUT_NODE_ROOT:
    case INPUT_NODE_DEVICE:
        return -EISDIR;
    case INPUT_NODE_INFO:
    case INPUT_NODE_EVENTS:
        return -EPERM;
#ifdef CONFIG_WANTED_VFS_INPUT_INJECT
    case INPUT_NODE_INJECT:
        return writeInject(d, f, buf, nbyte);
#endif
    default:
        return -ENOSYS;
    }
}

static int _Seek(vfs_driver_ctx_t d, int fd, long off, vfs_whence_t whence,
                 /* NOLINTNEXTLINE(readability-non-const-parameter) */
                 long *pos) {
    (void)off;
    (void)whence;
    (void)pos;
    return fdAt(d, fd) == NULL ? -EBADF : -ESPIPE;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct input_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == INPUT_NODE_DEVICE) {
        vfs_dir_entry_t nodes[2] = {
            {d->inject ? "info" : "events", VFS_FILETYPE_CHARACTER_DEVICE},
            {d->inject ? INPUT_INJECT_TOKEN : "info",
             VFS_FILETYPE_CHARACTER_DEVICE},
        };
        return VfsFlatDirReadDir(nodes, 2, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != INPUT_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_INPUT_MAX_DEVICES];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->devices[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
