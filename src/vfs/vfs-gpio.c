/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "vfs-internal.h"
#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* GPIO device driver: a per-pin subtree under /dev/gpio. <name> is a
 * wapp-visible label the grant maps onto a platform address, and ':' and ','
 * are reserved as field and entry separators. See the VFS reference. */

static const char id[] = {'G', 'p', 'i', 'o'};

#define GPIO_MAX_FDS 8
#define GPIO_ADDR_MAX 32
#define GPIO_NAME_MAX WAPP_MAX_NAME_LEN

/* Node kinds a descriptor can refer to. */
#define GPIO_NODE_ROOT 0
#define GPIO_NODE_PIN 1
#define GPIO_NODE_VALUE 2
#define GPIO_NODE_DIRECTION 3

struct gpio_pin_t {
    char name[GPIO_NAME_MAX + 1];
    char address[GPIO_ADDR_MAX];
    platform_gpio_t *line;
    bool output;
};

struct gpio_fd_t {
    bool used;
    uint8_t node;
    uint8_t pin;
    bool read_done; /* per-fd EOF latch */
};

struct vfs_driver_ctx_t {
    struct gpio_pin_t pins[CONFIG_WANTED_GPIO_MAX_PINS];
    uint8_t pin_cnt;
    struct gpio_fd_t fds[GPIO_MAX_FDS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

/* ── Grant parsing ───────────────────────────────────────────────────────── */

static const char PINS_KEY[] = "pins=";

/* Next `sep`-separated field, NUL-terminated in place. Advances *p past it.
 * Returns NULL once the string is consumed. An empty field is returned as ""
 * so the caller can reject it. */
static char *nextField(char **p, char sep) {
    char *s = *p;
    if (s == NULL)
        return NULL;
    if (*s == '\0') {
        *p = NULL;
        return NULL;
    }
    char *q = s;
    while (*q != '\0' && *q != sep)
        q++;
    if (*q == sep) {
        *q = '\0';
        *p = q + 1;
    } else {
        *p = q;
    }
    return s;
}

/* A pin label is [A-Za-z0-9_-], 1..GPIO_NAME_MAX characters. */
static bool validName(const char *s) {
    size_t n = 0;
    for (const char *p = s; *p != '\0'; p++, n++) {
        bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                  (*p >= '0' && *p <= '9') || *p == '_' || *p == '-';
        if (!ok)
            return false;
    }
    return n > 0 && n <= GPIO_NAME_MAX;
}

/* One entry: <name>:<address>:<direction>, then optional pull=, drive= and
 * init= fields. Fills `pin` and `cfg`. Returns 0 or -EINVAL. */
static int parseEntry(char *entry, struct gpio_pin_t *pin,
                      plat_gpio_cfg_t *cfg) {
    char *cur = entry;
    const char *name = nextField(&cur, ':');
    const char *addr = nextField(&cur, ':');
    const char *dir = nextField(&cur, ':');

    if (name == NULL || addr == NULL || dir == NULL)
        return -EINVAL;
    if (!validName(name))
        return -EINVAL;
    if (*addr == '\0' || strlen(addr) >= GPIO_ADDR_MAX)
        return -EINVAL;

    if (strcmp(dir, "out") == 0)
        cfg->direction = PLAT_GPIO_DIR_OUT;
    else if (strcmp(dir, "in") == 0)
        cfg->direction = PLAT_GPIO_DIR_IN;
    else
        return -EINVAL;

    cfg->pull = PLAT_GPIO_PULL_NONE;
    cfg->drive = PLAT_GPIO_DRIVE_PP;
    cfg->init = 0;
    bool sawInit = false;

    for (;;) {
        const char *extra = nextField(&cur, ':');
        if (extra == NULL)
            break;
        if (strcmp(extra, "pull=up") == 0)
            cfg->pull = PLAT_GPIO_PULL_UP;
        else if (strcmp(extra, "pull=down") == 0)
            cfg->pull = PLAT_GPIO_PULL_DOWN;
        else if (strcmp(extra, "pull=none") == 0)
            cfg->pull = PLAT_GPIO_PULL_NONE;
        else if (strcmp(extra, "drive=pp") == 0)
            cfg->drive = PLAT_GPIO_DRIVE_PP;
        else if (strcmp(extra, "drive=od") == 0)
            cfg->drive = PLAT_GPIO_DRIVE_OD;
        else if (strcmp(extra, "init=0") == 0) {
            cfg->init = 0;
            sawInit = true;
        } else if (strcmp(extra, "init=1") == 0) {
            cfg->init = 1;
            sawInit = true;
        }
        else
            return -EINVAL;
    }

    /* An input drives no level, so a grant that states one is a mistake. */
    if (sawInit && cfg->direction != PLAT_GPIO_DIR_OUT)
        return -EINVAL;

    strncpy(pin->name, name, sizeof(pin->name) - 1);
    strncpy(pin->address, addr, sizeof(pin->address) - 1);
    pin->output = (cfg->direction == PLAT_GPIO_DIR_OUT);
    cfg->address = pin->address;
    return 0;
}

/* Parse the whole pins= clause and open every line. Returns 0, or a negative
 * errno that fails the launch. On failure the caller destroys the context,
 * which closes whatever was already opened. */
static int parsePins(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(PINS_KEY) - 1;

    if (options == NULL || strncmp(options, PINS_KEY, keyLen) != 0) {
        DEBUG_TRACE("gpio grant has no pins= clause");
        return -EINVAL;
    }

    const char *list = options + keyLen;
    size_t len = strlen(list);
    if (len == 0) {
        DEBUG_TRACE("gpio grant has an empty pins= clause");
        return -EINVAL;
    }

    char *work = (char *)WantedMalloc(len + 1);
    if (work == NULL)
        return -ENOMEM;
    memcpy(work, list, len + 1);

    int rc = 0;
    char *cur = work;
    for (;;) {
        char *entry = nextField(&cur, ',');
        if (entry == NULL)
            break;
        if (*entry == '\0') {
            rc = -EINVAL;
            break;
        }
        if (ctx->pin_cnt >= CONFIG_WANTED_GPIO_MAX_PINS) {
            DEBUG_TRACE("gpio grant exceeds %d pins",
                        CONFIG_WANTED_GPIO_MAX_PINS);
            rc = -E2BIG;
            break;
        }

        struct gpio_pin_t *pin = &ctx->pins[ctx->pin_cnt];
        plat_gpio_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        rc = parseEntry(entry, pin, &cfg);
        if (rc < 0) {
            DEBUG_TRACE("gpio grant entry is malformed");
            break;
        }

        /* A duplicate label or a duplicate address inside one grant is
         * rejected: two names for one line would let a wapp hold it twice. */
        for (uint8_t i = 0; i < ctx->pin_cnt; i++) {
            if (strcmp(ctx->pins[i].name, pin->name) == 0 ||
                strcmp(ctx->pins[i].address, pin->address) == 0) {
                DEBUG_TRACE("gpio grant repeats %s", pin->name);
                rc = -EINVAL;
                break;
            }
        }
        if (rc < 0)
            break;

        rc = PlatformGpioOpen(&cfg, &pin->line);
        if (rc < 0) {
            DEBUG_TRACE("gpio %s: backing refused the line (%d)", pin->name,
                        rc);
            break;
        }
        ctx->pin_cnt++;
    }

    WantedFree(work);
    return rc;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static int findPin(const struct vfs_driver_ctx_t *ctx, const char *name,
                   size_t nameLen) {
    for (uint8_t i = 0; i < ctx->pin_cnt; i++) {
        if (strlen(ctx->pins[i].name) == nameLen &&
            strncmp(ctx->pins[i].name, name, nameLen) == 0)
            return i;
    }
    return -1;
}

/* Resolve a driver-relative path to a node kind and pin index. */
static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *pin) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = GPIO_NODE_ROOT;
        *pin = 0;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;

    int idx = findPin(ctx, p, (size_t)(slash - p));
    if (idx < 0)
        return -ENOENT;
    *pin = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0') {
        *node = GPIO_NODE_PIN;
        return 0;
    }
    if (strcmp(slash, "value") == 0) {
        *node = GPIO_NODE_VALUE;
        return 0;
    }
    if (strcmp(slash, "direction") == 0) {
        *node = GPIO_NODE_DIRECTION;
        return 0;
    }
    return -ENOENT;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

vfs_driver_t *VfsGpioInit(const wapp_t *wapp, const char *options) {
    (void)wapp;

    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (NULL == driver) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (NULL == ctx) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
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
    driver->ReadDir = _ReadDir;

    if (parsePins(ctx, options) < 0) {
        _Destroy(driver);
        return NULL;
    }

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        for (uint8_t i = 0; i < ctx->pin_cnt; i++)
            PlatformGpioClose(ctx->pins[i].line);
        WantedFree(ctx);
    }
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    uint8_t node;
    uint8_t pin;
    int rc = resolve(d, path != NULL ? path : "", &node, &pin);
    if (rc < 0)
        return rc;

    int slot = -1;
    for (int i = 0; i < GPIO_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            slot = i;
            break;
        }
    }
    if (slot < 0)
        return -EMFILE;

    d->fds[slot].used = true;
    d->fds[slot].node = node;
    d->fds[slot].pin = pin;
    d->fds[slot].read_done = false;
    return slot;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    uint8_t node = d->fds[fd].node;
    s->filetype = (node == GPIO_NODE_ROOT || node == GPIO_NODE_PIN)
                      ? VFS_FILETYPE_DIRECTORY
                      : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* Copy one text line out, then latch EOF on this descriptor. The value
 * regenerates on a fresh open. */
static int emit(struct gpio_fd_t *f, const char *line, size_t lineLen,
                void *buf, size_t nbyte) {
    size_t n = (nbyte < lineLen) ? nbyte : lineLen;
    memcpy(buf, line, n);
    f->read_done = true;
    return (int)n;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct gpio_fd_t *f = &d->fds[fd];
    if (f->node == GPIO_NODE_ROOT || f->node == GPIO_NODE_PIN)
        return -EISDIR;
    if (f->read_done)
        return 0;

    if (f->node == GPIO_NODE_DIRECTION) {
        const char *line = d->pins[f->pin].output ? "out\n" : "in\n";
        return emit(f, line, strlen(line), buf, nbyte);
    }

    bool level = false;
    int rc = PlatformGpioRead(d->pins[f->pin].line, &level);
    if (rc < 0)
        return rc;

    char line[2];
    line[0] = level ? '1' : '0';
    line[1] = '\n';
    return emit(f, line, sizeof(line), buf, nbyte);
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct gpio_fd_t *f = &d->fds[fd];
    if (f->node == GPIO_NODE_ROOT || f->node == GPIO_NODE_PIN)
        return -EISDIR;
    /* direction reports the grant; it does not set it. */
    if (f->node == GPIO_NODE_DIRECTION)
        return -EPERM;
    if (!d->pins[f->pin].output)
        return -EPERM;

    /* "0" or "1", with the trailing newline a read of this node emits. */
    const char *p = buf;
    if (nbyte == 0 || (nbyte > 1 && !(nbyte == 2 && p[1] == '\n')))
        return -EINVAL;
    if (p[0] != '0' && p[0] != '1')
        return -EINVAL;

    int rc = PlatformGpioWrite(d->pins[f->pin].line, p[0] == '1');
    if (rc < 0)
        return rc;
    return (int)nbyte;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (fd < 0 || fd >= GPIO_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    const struct gpio_fd_t *f = &d->fds[fd];

    if (f->node == GPIO_NODE_PIN) {
        static const vfs_dir_entry_t attrs[] = {
            {"value", VFS_FILETYPE_CHARACTER_DEVICE},
            {"direction", VFS_FILETYPE_CHARACTER_DEVICE},
        };
        return VfsFlatDirReadDir(attrs, 2, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != GPIO_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_GPIO_MAX_PINS];
    for (uint8_t i = 0; i < d->pin_cnt; i++) {
        entries[i].name = d->pins[i].name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->pin_cnt, buf, bufLen, cookie, bufUsed);
}
