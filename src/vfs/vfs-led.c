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
#include <vfs-led.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

#ifdef CONFIG_WANTED_VFS_INPUT
#include <vfs-input.h>
#endif

/* LED driver: /dev/led/<name>/{brightness,max_brightness,trigger,idle_ms,
 * idle_level,ctl}, one subtree per LED. LEDs live in an engine-wide table, so
 * the input core and the engine loop reach the triggers. */

static const char id[] = {'L', 'e', 'D', 'v'};

#define LED_MAX_FDS 8
#define LED_LEDS_KEY "leds="
#define LED_ACTIVITY_KEY "activity="
#define LED_MODE_PWM "pwm"
#define LED_MODE_ONOFF "onoff"
#define LED_FADE_VERB "fade"

#define LED_NODE_ROOT 0
#define LED_NODE_DEVICE 1
#define LED_NODE_BRIGHTNESS 2
#define LED_NODE_MAX 3
#define LED_NODE_TRIGGER 4
#define LED_NODE_IDLE_MS 5
#define LED_NODE_IDLE_LEVEL 6
#define LED_NODE_CTL 7

#define LED_TRIGGER_NONE 0
#define LED_TRIGGER_HEARTBEAT 1
#define LED_TRIGGER_ACTIVITY 2

#define LED_LINE_MAX 32
#define LED_DIGITS_MAX 9
#define LED_IDLE_MS_DEFAULT 10000U
#define LED_IDLE_MS_MAX 86400000U
#define LED_FADE_MS_MAX 3600000U
#define LED_HEARTBEAT_PERIOD_MS 5000U
#define LED_HEARTBEAT_FADE_MS 500U
#define LED_IDLE_FADE_MS 1000U
#define LED_WAKE_FADE_MS 100U
#define LED_NS_PER_MS 1000000ULL
#define LED_ENTRY_FIELDS 3

static const char *const triggerNames[] = {"none", "heartbeat", "activity"};
#define LED_TRIGGER_COUNT (sizeof(triggerNames) / sizeof(triggerNames[0]))

typedef struct led_dev_t {
    bool used;
    bool owned;
    bool generic; /* a pin LED of a live grant, freed with it */
    char name[LED_NAME_MAX + 1];
    unsigned max;
    bool hwFade;
    led_ops_t ops;
    void *ctx;
    platform_led_t *pin;
    uint8_t trigger;
    uint32_t idleMs;
    unsigned idleLevel;
    const void *input; /* the input device the activity trigger watches */
    unsigned target;   /* activity: the level held while not idle */
    bool idled;
    uint64_t activityMs;
    bool beatDue;
    uint8_t beatPhase; /* 1 while the beat is up */
    uint64_t beatMs;
} led_dev_t;

static struct {
    led_dev_t devs[CONFIG_WANTED_LED_MAX_DEVICES];
    platform_mutex_t *lock;
} table;

struct led_fd_t {
    bool used;
    bool done;
    uint8_t node;
    uint8_t dev;
};

struct vfs_driver_ctx_t {
    led_dev_t *devs[CONFIG_WANTED_LED_MAX_DEVICES];
    uint8_t count;
    struct led_fd_t fds[LED_MAX_FDS];
};

/* ── Pin LEDs ────────────────────────────────────────────────────────────── */

static int pinSet(void *ctx, unsigned level) {
    return PlatformLedSet(ctx, level);
}

static int pinFade(void *ctx, unsigned level, unsigned ms) {
    return PlatformLedFade(ctx, level, ms);
}

static unsigned pinGet(const void *ctx) { return PlatformLedGet(ctx); }

static const led_ops_t pinOps = {pinSet, pinFade, pinGet};

/* ── Device table ────────────────────────────────────────────────────────── */

static uint64_t nowMs(void) {
    plat_timestamp_t ns = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &ns) < 0)
        return 0;
    return ns / LED_NS_PER_MS;
}

/* [A-Za-z0-9_-], 1..LED_NAME_MAX characters. */
static bool validToken(const char *s, size_t len) {
    if (len == 0 || len > LED_NAME_MAX)
        return false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok)
            return false;
    }
    return true;
}

static led_dev_t *findDevice(const char *name, size_t len) {
    for (size_t i = 0; i < CONFIG_WANTED_LED_MAX_DEVICES; i++) {
        led_dev_t *d = &table.devs[i];
        if (d->used && strlen(d->name) == len &&
            strncmp(d->name, name, len) == 0)
            return d;
    }
    return NULL;
}

static led_dev_t *freeSlot(void) {
    for (size_t i = 0; i < CONFIG_WANTED_LED_MAX_DEVICES; i++) {
        if (!table.devs[i].used)
            return &table.devs[i];
    }
    return NULL;
}

/* The state a grant owns; the level stays where it is. */
static void resetClaim(led_dev_t *d) {
    d->trigger = LED_TRIGGER_NONE;
    d->idleMs = LED_IDLE_MS_DEFAULT;
    d->idleLevel = 0;
    d->input = NULL;
    d->target = 0;
    d->idled = false;
    d->beatDue = false;
    d->beatPhase = 0;
}

int LedDeviceRegister(const led_device_desc_t *desc) {
    if (desc == NULL || desc->name == NULL ||
        !validToken(desc->name, strlen(desc->name)) || desc->max < 1 ||
        desc->ops == NULL || desc->ops->set == NULL ||
        desc->ops->fade == NULL || desc->ops->get == NULL)
        return -EINVAL;

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = 0;
    led_dev_t *d = NULL;
    if (findDevice(desc->name, strlen(desc->name)) != NULL) {
        rc = -EEXIST;
    } else {
        d = freeSlot();
        if (d == NULL)
            rc = -ENOSPC;
    }
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
        d->used = true;
        strncpy(d->name, desc->name, LED_NAME_MAX);
        d->max = desc->max;
        d->hwFade = desc->hwFade;
        d->ops = *desc->ops;
        d->ctx = desc->ctx;
        resetClaim(d);
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

void LedDevicesReset(void) {
    for (size_t i = 0; i < CONFIG_WANTED_LED_MAX_DEVICES; i++) {
        if (table.devs[i].used && table.devs[i].generic)
            PlatformLedClose(table.devs[i].pin);
    }
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

/* ── Triggers ────────────────────────────────────────────────────────────── */

/* The level the LED holds is the one last written while the activity trigger
 * runs, and a write counts as activity. Caller holds the lock. */
static void holdLevel(led_dev_t *d, unsigned level) {
    if (d->trigger != LED_TRIGGER_ACTIVITY)
        return;
    d->target = level;
    d->idled = false;
    d->activityMs = nowMs();
}

static void tickHeartbeat(led_dev_t *d, uint64_t now) {
    if (d->beatPhase == 1) {
        d->beatPhase = 0;
        if (d->hwFade)
            d->ops.fade(d->ctx, 0, LED_HEARTBEAT_FADE_MS);
        else
            d->ops.set(d->ctx, 0);
    } else if (d->beatDue || now - d->beatMs >= LED_HEARTBEAT_PERIOD_MS) {
        d->beatDue = false;
        d->beatMs = now;
        d->beatPhase = 1;
        if (d->hwFade)
            d->ops.fade(d->ctx, d->max, LED_HEARTBEAT_FADE_MS);
        else
            d->ops.set(d->ctx, d->max);
    }
}

static void tickActivity(led_dev_t *d, uint64_t now) {
    if (d->idled || d->idleMs == 0 || now - d->activityMs < d->idleMs)
        return;
    d->idled = true;
    unsigned level = d->idleLevel < d->target ? d->idleLevel : d->target;
    d->ops.fade(d->ctx, level, LED_IDLE_FADE_MS);
}

void LedTick(void) {
    PlatformMutexLock(table.lock);
    uint64_t now = nowMs();
    for (size_t i = 0; i < CONFIG_WANTED_LED_MAX_DEVICES; i++) {
        led_dev_t *d = &table.devs[i];
        if (!d->used || !d->owned)
            continue;
        if (d->trigger == LED_TRIGGER_HEARTBEAT)
            tickHeartbeat(d, now);
        else if (d->trigger == LED_TRIGGER_ACTIVITY)
            tickActivity(d, now);
    }
    PlatformMutexUnlock(table.lock);
}

#ifdef CONFIG_WANTED_VFS_INPUT
void LedActivity(const input_device_t *dev) {
    PlatformMutexLock(table.lock);
    uint64_t now = nowMs();
    for (size_t i = 0; i < CONFIG_WANTED_LED_MAX_DEVICES; i++) {
        led_dev_t *d = &table.devs[i];
        if (!d->used || !d->owned || d->trigger != LED_TRIGGER_ACTIVITY ||
            d->input != dev)
            continue;
        d->activityMs = now;
        if (d->idled) {
            d->idled = false;
            d->ops.fade(d->ctx, d->target, LED_WAKE_FADE_MS);
        }
    }
    PlatformMutexUnlock(table.lock);
}
#endif

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Split `s` at the first `sep` into the text before it, which stays in `s`,
 * and the rest, which is returned. NULL when there is no separator. */
/* cppcheck-suppress constParameterPointer ; the returned pointer is written */
static char *splitAt(char *s, char sep) {
    char *p = strchr(s, sep);
    if (p == NULL)
        return NULL;
    *p = '\0';
    return p + 1;
}

/* A bare name selects a device the board registered. */
static int claimBoardDevice(struct vfs_driver_ctx_t *ctx, const char *name) {
    led_dev_t *d = findDevice(name, strlen(name));
    if (d == NULL || d->generic) {
        DEBUG_TRACE("led grant names an unknown device");
        return -EINVAL;
    }
    if (d->owned)
        return -EBUSY;
    d->owned = true;
    resetClaim(d);
    ctx->devs[ctx->count++] = d;
    return 0;
}

/* name:address:mode opens a pin LED through the platform backing. */
static int openPinLed(struct vfs_driver_ctx_t *ctx, const char *name,
                      char *rest) {
    const char *mode = splitAt(rest, ':');
    if (mode == NULL || strchr(mode, ':') != NULL || rest[0] == '\0')
        return -EINVAL;
    if (!validToken(name, strlen(name)))
        return -EINVAL;

    plat_led_cfg_t cfg = {rest, PLAT_LED_MODE_PWM};
    if (strcmp(mode, LED_MODE_ONOFF) == 0)
        cfg.mode = PLAT_LED_MODE_ONOFF;
    else if (strcmp(mode, LED_MODE_PWM) != 0)
        return -EINVAL;

    led_dev_t *d = findDevice(name, strlen(name));
    if (d != NULL)
        return d->owned ? -EBUSY : -EINVAL;
    if ((d = freeSlot()) == NULL)
        return -ENOSPC;

    platform_led_t *pin = NULL;
    int rc = PlatformLedOpen(&cfg, &pin);
    if (rc < 0) {
        DEBUG_TRACE("led %s: backing refused the address (%d)", name, rc);
        return rc;
    }
    memset(d, 0, sizeof(*d));
    d->used = true;
    d->owned = true;
    d->generic = true;
    strncpy(d->name, name, LED_NAME_MAX);
    d->pin = pin;
    d->max = PlatformLedMax(pin);
    d->hwFade = PlatformLedHwFade(pin);
    d->ops = pinOps;
    d->ctx = pin;
    resetClaim(d);
    ctx->devs[ctx->count++] = d;
    return 0;
}

static int addEntry(struct vfs_driver_ctx_t *ctx, char *entry) {
    if (ctx->count >= CONFIG_WANTED_LED_MAX_DEVICES)
        return -E2BIG;
    char *rest = splitAt(entry, ':');
    if (rest == NULL) {
        if (!validToken(entry, strlen(entry)))
            return -EINVAL;
        return claimBoardDevice(ctx, entry);
    }
    return openPinLed(ctx, entry, rest);
}

/* activity=<led>:<input> ties a granted LED to an input device. */
static int linkActivity(struct vfs_driver_ctx_t *ctx, char *clause) {
#ifdef CONFIG_WANTED_VFS_INPUT
    char *inputName = splitAt(clause, ':');
    if (inputName == NULL || clause[0] == '\0' || inputName[0] == '\0')
        return -EINVAL;

    led_dev_t *led = NULL;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->devs[i]->name, clause) == 0)
            led = ctx->devs[i];
    }
    if (led == NULL || led->input != NULL)
        return -EINVAL;

    const input_device_t *in = InputDeviceFind(inputName);
    if (in == NULL)
        return -EINVAL;
    led->input = in;
    return 0;
#else
    (void)ctx;
    (void)clause;
    return -EINVAL;
#endif
}

/* Parse leds=<entry>[,<entry>...][,activity=<led>:<input>...]. Entries open
 * first, in any position; the activity clauses then name them. Returns 0 or a
 * negative errno; the caller destroys the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(LED_LEDS_KEY) - 1;
    const size_t actLen = sizeof(LED_ACTIVITY_KEY) - 1;

    if (options == NULL || strncmp(options, LED_LEDS_KEY, keyLen) != 0 ||
        options[keyLen] == '\0') {
        DEBUG_TRACE("led grant has no leds= clause");
        return -EINVAL;
    }

    size_t len = strlen(options + keyLen);
    char *work = WantedMalloc(len + 1);
    if (work == NULL)
        return -ENOMEM;
    memcpy(work, options + keyLen, len + 1);

    char *clauses[CONFIG_WANTED_LED_MAX_DEVICES];
    size_t nClauses = 0;
    int rc = 0;
    char *cur = work;
    while (cur != NULL && rc == 0) {
        char *item = cur;
        cur = splitAt(cur, ',');
        if (item[0] == '\0') {
            rc = -EINVAL;
        } else if (strncmp(item, LED_ACTIVITY_KEY, actLen) == 0) {
            if (nClauses >= CONFIG_WANTED_LED_MAX_DEVICES)
                rc = -E2BIG;
            else
                clauses[nClauses++] = item + actLen;
        } else {
            rc = addEntry(ctx, item);
        }
    }
    for (size_t i = 0; i < nClauses && rc == 0; i++)
        rc = linkActivity(ctx, clauses[i]);

    WantedFree(work);
    return rc;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static const struct {
    const char *name;
    uint8_t node;
} nodes[] = {
    {"brightness", LED_NODE_BRIGHTNESS}, {"max_brightness", LED_NODE_MAX},
    {"trigger", LED_NODE_TRIGGER},       {"idle_ms", LED_NODE_IDLE_MS},
    {"idle_level", LED_NODE_IDLE_LEVEL}, {"ctl", LED_NODE_CTL},
};
#define LED_NODE_COUNT (sizeof(nodes) / sizeof(nodes[0]))

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *dev) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = LED_NODE_ROOT;
        *dev = 0;
        return 0;
    }

    const char *slash = p;
    while (*slash != '\0' && *slash != '/')
        slash++;
    size_t nameLen = (size_t)(slash - p);

    int idx = -1;
    for (uint8_t i = 0; i < ctx->count; i++) {
        if (strlen(ctx->devs[i]->name) == nameLen &&
            strncmp(ctx->devs[i]->name, p, nameLen) == 0)
            idx = i;
    }
    if (idx < 0)
        return -ENOENT;
    *dev = (uint8_t)idx;

    while (*slash == '/')
        slash++;
    if (*slash == '\0') {
        *node = LED_NODE_DEVICE;
        return 0;
    }
    for (size_t i = 0; i < LED_NODE_COUNT; i++) {
        if (strcmp(slash, nodes[i].name) == 0) {
            *node = nodes[i].node;
            return 0;
        }
    }
    return -ENOENT;
}

/* ── Driver lifecycle ────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsLedInit(const wapp_t *wapp, const char *options) {
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

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = parseGrant(ctx, options);
    PlatformMutexUnlock(table.lock);
    if (rc < 0) {
        _Destroy(driver);
        return NULL;
    }
    return driver;
}

/* A pin LED goes with its grant. A board device stays registered and unowned.
 */
static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        PlatformMutexLock(table.lock);
        for (uint8_t i = 0; i < ctx->count; i++) {
            led_dev_t *led = ctx->devs[i];
            if (led->generic) {
                PlatformLedClose(led->pin);
                memset(led, 0, sizeof(*led));
            } else {
                led->owned = false;
            }
        }
        PlatformMutexUnlock(table.lock);
        WantedFree(ctx);
    }
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct led_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= LED_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;

    uint8_t node;
    uint8_t dev;
    int rc = resolve(d, path != NULL ? path : "", &node, &dev);
    if (rc < 0)
        return rc;

    for (int i = 0; i < LED_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            d->fds[i].dev = dev;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct led_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct led_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == LED_NODE_ROOT || f->node == LED_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* One text line, then EOF on this descriptor. Each open reads afresh. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct led_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == LED_NODE_ROOT || f->node == LED_NODE_DEVICE)
        return -EISDIR;
    if (f->node == LED_NODE_CTL)
        return -EPERM;
    if (f->done)
        return 0;

    led_dev_t *led = d->devs[f->dev];
    char line[LED_LINE_MAX];
    PlatformMutexLock(table.lock);
    switch (f->node) {
    case LED_NODE_BRIGHTNESS:
        snprintf(line, sizeof(line), "%u\n", led->ops.get(led->ctx));
        break;
    case LED_NODE_MAX:
        snprintf(line, sizeof(line), "%u\n", led->max);
        break;
    case LED_NODE_TRIGGER:
        snprintf(line, sizeof(line), "%s\n", triggerNames[led->trigger]);
        break;
    case LED_NODE_IDLE_MS:
        snprintf(line, sizeof(line), "%u\n", (unsigned)led->idleMs);
        break;
    default:
        snprintf(line, sizeof(line), "%u\n", led->idleLevel);
        break;
    }
    PlatformMutexUnlock(table.lock);

    size_t len = strlen(line);
    size_t n = nbyte < len ? nbyte : len;
    memcpy(buf, line, n);
    f->done = true;
    return (int)n;
}

/* Decimal digits only, so a sign or a space is an error. */
static int parseNumber(const char *s, unsigned long *out) {
    size_t n = strlen(s);
    if (n == 0 || n > LED_DIGITS_MAX)
        return -EINVAL;
    unsigned long v = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -EINVAL;
        v = v * 10 + (unsigned long)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int writeBrightness(led_dev_t *led, const char *line) {
    unsigned long v;
    if (parseNumber(line, &v) < 0 || v > led->max)
        return -EINVAL;
    if (led->trigger == LED_TRIGGER_HEARTBEAT)
        return -EBUSY;
    int rc = led->ops.set(led->ctx, (unsigned)v);
    if (rc == 0)
        holdLevel(led, (unsigned)v);
    return rc;
}

/* "fade <level> <ms>". */
static int writeCtl(led_dev_t *led, char *line) {
    char *level = splitAt(line, ' ');
    const char *ms = level != NULL ? splitAt(level, ' ') : NULL;
    unsigned long lv;
    unsigned long dur;
    if (ms == NULL || strcmp(line, LED_FADE_VERB) != 0 ||
        parseNumber(level, &lv) < 0 || parseNumber(ms, &dur) < 0 ||
        lv > led->max || dur > LED_FADE_MS_MAX)
        return -EINVAL;
    if (led->trigger == LED_TRIGGER_HEARTBEAT)
        return -EBUSY;

    int rc = dur == 0 ? led->ops.set(led->ctx, (unsigned)lv)
                      : led->ops.fade(led->ctx, (unsigned)lv, (unsigned)dur);
    if (rc == 0)
        holdLevel(led, (unsigned)lv);
    return rc;
}

static int writeTrigger(led_dev_t *led, const char *line) {
    size_t want = LED_TRIGGER_COUNT;
    for (size_t i = 0; i < LED_TRIGGER_COUNT; i++) {
        if (strcmp(line, triggerNames[i]) == 0)
            want = i;
    }
    if (want == LED_TRIGGER_COUNT)
        return -EINVAL;
    if (want == LED_TRIGGER_ACTIVITY && led->input == NULL)
        return -EINVAL;

    if (led->trigger == LED_TRIGGER_HEARTBEAT &&
        want != LED_TRIGGER_HEARTBEAT) {
        led->ops.set(led->ctx, 0);
        led->beatPhase = 0;
    }
    led->trigger = (uint8_t)want;
    if (want == LED_TRIGGER_HEARTBEAT) {
        led->beatDue = true;
        led->beatPhase = 0;
    } else if (want == LED_TRIGGER_ACTIVITY) {
        led->target = led->ops.get(led->ctx);
        led->idled = false;
        led->activityMs = nowMs();
    }
    return 0;
}

static int writeLine(led_dev_t *led, uint8_t node, char *line) {
    unsigned long v;
    switch (node) {
    case LED_NODE_BRIGHTNESS:
        return writeBrightness(led, line);
    case LED_NODE_TRIGGER:
        return writeTrigger(led, line);
    case LED_NODE_CTL:
        return writeCtl(led, line);
    case LED_NODE_IDLE_MS:
        if (parseNumber(line, &v) < 0 || v > LED_IDLE_MS_MAX)
            return -EINVAL;
        led->idleMs = (uint32_t)v;
        return 0;
    default:
        if (parseNumber(line, &v) < 0 || v > led->max)
            return -EINVAL;
        led->idleLevel = (unsigned)v;
        return 0;
    }
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct led_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == LED_NODE_ROOT || f->node == LED_NODE_DEVICE)
        return -EISDIR;
    if (f->node == LED_NODE_MAX)
        return -EPERM;

    /* One line, with or without the newline a read of the node ends in. */
    if (nbyte == 0 || nbyte >= LED_LINE_MAX)
        return -EINVAL;
    char line[LED_LINE_MAX];
    memcpy(line, buf, nbyte);
    line[nbyte] = '\0';
    if (line[nbyte - 1] == '\n')
        line[nbyte - 1] = '\0';

    PlatformMutexLock(table.lock);
    int rc = writeLine(d->devs[f->dev], f->node, line);
    PlatformMutexUnlock(table.lock);
    return rc < 0 ? rc : (int)nbyte;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct led_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == LED_NODE_DEVICE) {
        vfs_dir_entry_t attrs[LED_NODE_COUNT];
        for (size_t i = 0; i < LED_NODE_COUNT; i++) {
            attrs[i].name = nodes[i].name;
            attrs[i].type = VFS_FILETYPE_CHARACTER_DEVICE;
        }
        return VfsFlatDirReadDir(attrs, LED_NODE_COUNT, buf, bufLen, cookie,
                                 bufUsed);
    }
    if (f->node != LED_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_LED_MAX_DEVICES];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->devs[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
