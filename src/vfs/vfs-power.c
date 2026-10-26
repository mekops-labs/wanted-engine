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
#include <vfs-power.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-autoconf.h>
#include <wanted_malloc.h>

/* Power driver: /dev/power/<name>/{policy,mode,event,wake,caps,rails,ctl}.
 * Devices, the ladder and the clients live in an engine-wide table, so the
 * ladder outlives the wapp that declared it and runs with no wapp at all. */

static const char id[] = {'P', 'o', 'W', 'r'};

#define POWER_MAX_FDS 8
#define POWER_DEVICES_KEY "devices="
#define POWER_CONTROL_TOKEN "control"

#define POWER_NODE_ROOT 0
#define POWER_NODE_DEVICE 1
#define POWER_NODE_POLICY 2
#define POWER_NODE_MODE 3
#define POWER_NODE_EVENT 4
#define POWER_NODE_WAKE 5
#define POWER_NODE_CAPS 6
#define POWER_NODE_RAILS 7
#define POWER_NODE_CTL 8

#define POWER_POLICY_MAX 256
#define POWER_EVENTS 8
#define POWER_EVENT_MAX 40
#define POWER_LINE_MAX 48
#define POWER_TEXT_MAX 256
#define POWER_DIGITS_MAX 9
#define POWER_MS_PER_S 1000ULL
#define POWER_US_PER_MS 1000ULL
#define POWER_NS_PER_MS 1000000ULL
#define POWER_GRACE_DEFAULT_MS 10000ULL
#define POWER_GRACE_SAFETY_MS 3000ULL
/* How long a ladder wake keeps the hardware down waiting for the next mode. */
#define POWER_LADDER_SLACK_MS 1000ULL
#define POWER_SLEEP_NS 1000000ULL /* 1 ms */
#define POWER_PERSIST_MAGIC 0x50575231U

#define POWER_ARMED_NONE 0
#define POWER_ARMED_LADDER 1
#define POWER_ARMED_ALARM 2

static const char *const modeNames[POWER_MODE_COUNT] = {
    "active", "standby", "suspend", "sleep", "off"};

typedef struct policy_line_t {
    bool used;
    bool hasIdle;
    bool hasAfter;
    bool hasAlarm;
    bool hasGrace;
    bool battery;
    uint8_t resume;
    unsigned wake;
    uint32_t idleS;
    uint32_t afterS;
    uint32_t alarmS;
    uint32_t graceS;
} policy_line_t;

typedef struct power_dev_t {
    bool used;
    bool controlOwned;
    bool busy; /* a transition's hooks run, with the lock dropped */
    char name[POWER_NAME_MAX + 1];
    power_device_desc_t desc;
    uint8_t mode;
    uint64_t sinceMs;
    uint64_t activityMs;
    policy_line_t pol[POWER_MODE_COUNT];
    char policyText[POWER_POLICY_MAX];
    bool pending;
    bool ready;
    bool forced;  /* a safety transition: it overrides a policy */
    bool reenter; /* the pending mode is the one the device is in */
    uint8_t target;
    uint64_t dueMs;
    uint32_t alarmOverrideS;
    uint8_t armed;
    bool down; /* a ladder wake left the board and the clients suspended */
    uint64_t downUntilMs;
    char wake[POWER_WORD_MAX + 1];
    char events[POWER_EVENTS][POWER_EVENT_MAX];
    uint8_t evHead;
    uint8_t evCount;
} power_dev_t;

static struct {
    power_dev_t devs[CONFIG_WANTED_POWER_MAX_DEVICES];
    power_client_t clients[CONFIG_WANTED_POWER_MAX_CLIENTS];
    uint8_t clientCount;
    platform_mutex_t *lock;
} table;

typedef struct persist_t {
    uint32_t magic;
    char name[POWER_NAME_MAX + 1];
    uint8_t mode;
    uint8_t armed;
    uint32_t sinceAgeS;
    uint32_t activityAgeS;
    char policyText[POWER_POLICY_MAX];
} persist_t;

struct power_fd_t {
    bool used;
    uint8_t node;
    uint8_t dev;
    bool done;
    bool nonblock;
};

struct vfs_driver_ctx_t {
    power_dev_t *devs[CONFIG_WANTED_POWER_MAX_DEVICES];
    uint8_t count;
    bool control;
    int wakeFd;
    struct power_fd_t fds[POWER_MAX_FDS];
};

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint64_t nowMs(void) {
    plat_timestamp_t ns = 0;
    if (PlatformClockGetTime(PLAT_CLOCKID_MONOTONIC, &ns) < 0)
        return 0;
    return ns / POWER_NS_PER_MS;
}

/* [A-Za-z0-9_-], 1..max characters. */
static bool validToken(const char *s, size_t len, size_t max) {
    if (len == 0 || len > max)
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

/* Decimal digits only, so a sign or a space is an error. */
static int parseNumber(const char *s, size_t len, uint32_t *out) {
    if (len == 0 || len > POWER_DIGITS_MAX)
        return -EINVAL;
    uint32_t v = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return -EINVAL;
        v = v * 10 + (uint32_t)(s[i] - '0');
    }
    *out = v;
    return 0;
}

static int findMode(const char *s, size_t len) {
    for (int m = 0; m < POWER_MODE_COUNT; m++) {
        if (strlen(modeNames[m]) == len && strncmp(modeNames[m], s, len) == 0)
            return m;
    }
    return -1;
}

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

static power_dev_t *findDevice(const char *name, size_t len) {
    for (size_t i = 0; i < CONFIG_WANTED_POWER_MAX_DEVICES; i++) {
        power_dev_t *d = &table.devs[i];
        if (d->used && strlen(d->name) == len &&
            strncmp(d->name, name, len) == 0)
            return d;
    }
    return NULL;
}

static bool modeAvailable(const power_dev_t *d, unsigned mode) {
    if (mode == POWER_ACTIVE || mode == POWER_OFF)
        return true;
    return (d->desc.modes & POWER_BIT(mode)) != 0;
}

static bool externalPower(const power_dev_t *d) {
    return d->desc.ops != NULL && d->desc.ops->externalPower != NULL &&
           d->desc.ops->externalPower(d->desc.ctx);
}

/* ── Events ──────────────────────────────────────────────────────────────── */

/* Caller holds the lock. A full queue drops its oldest line. */
static void pushEvent(power_dev_t *d, const char *kind, unsigned mode,
                      const char *cause) {
    uint8_t slot = (uint8_t)((d->evHead + d->evCount) % POWER_EVENTS);
    if (d->evCount == POWER_EVENTS) {
        slot = d->evHead;
        d->evHead = (uint8_t)((d->evHead + 1) % POWER_EVENTS);
    } else {
        d->evCount++;
    }
    if (cause != NULL)
        snprintf(d->events[slot], POWER_EVENT_MAX, "%s %s %s\n", kind,
                 modeNames[mode], cause);
    else
        snprintf(d->events[slot], POWER_EVENT_MAX, "%s %s\n", kind,
                 modeNames[mode]);
}

static size_t eventBytes(const power_dev_t *d) {
    size_t n = 0;
    for (uint8_t i = 0; i < d->evCount; i++)
        n += strlen(d->events[(d->evHead + i) % POWER_EVENTS]);
    return n;
}

/* Pop whole lines into `buf`; returns the bytes copied. Caller holds the lock.
 */
static size_t popEvents(power_dev_t *d, char *buf, size_t nbyte) {
    size_t used = 0;
    while (d->evCount > 0) {
        const char *line = d->events[d->evHead];
        size_t len = strlen(line);
        if (used + len > nbyte)
            break;
        memcpy(buf + used, line, len);
        used += len;
        d->evHead = (uint8_t)((d->evHead + 1) % POWER_EVENTS);
        d->evCount--;
    }
    return used;
}

/* ── Policy ──────────────────────────────────────────────────────────────── */

static void setWake(power_dev_t *d, const char *word) {
    strncpy(d->wake, word, POWER_WORD_MAX);
    d->wake[POWER_WORD_MAX] = '\0';
}

/* The bit of a wake word in the device's table, or -1. */
static int wakeIndex(const power_dev_t *d, const char *s, size_t len) {
    for (unsigned i = 0; i < d->desc.wakeCount; i++) {
        const char *name = d->desc.wakes[i].name;
        if (strlen(name) == len && strncmp(name, s, len) == 0)
            return (int)i;
    }
    return -1;
}

static int parseWakeList(const power_dev_t *d, unsigned mode, char *list,
                         unsigned *mask) {
    unsigned out = 0;
    char *cur = list;
    while (cur != NULL) {
        char *item = cur;
        cur = splitAt(cur, ',');
        int idx = wakeIndex(d, item, strlen(item));
        if (idx < 0 || (d->desc.wakes[idx].modes & POWER_BIT(mode)) == 0)
            return -EINVAL;
        out |= 1U << idx;
    }
    *mask = out;
    return 0;
}

/* One term of a policy line. */
static int parseTerm(const power_dev_t *d, unsigned mode, policy_line_t *p,
                     char *term) {
    char *value = splitAt(term, '=');
    uint32_t n;
    if (value == NULL) {
        if (strcmp(term, "battery") != 0 || p->battery)
            return -EINVAL;
        p->battery = true;
        return 0;
    }
    if (strcmp(term, "wake") == 0)
        return p->wake != 0 || value[0] == '\0'
                   ? -EINVAL
                   : parseWakeList(d, mode, value, &p->wake);
    if (strcmp(term, "resume") == 0) {
        int m = findMode(value, strlen(value));
        if (m != POWER_ACTIVE && m != POWER_STANDBY)
            return -EINVAL;
        p->resume = (uint8_t)m;
        return 0;
    }
    if (parseNumber(value, strlen(value), &n) < 0)
        return -EINVAL;
    if (strcmp(term, "idle") == 0 && !p->hasIdle && n > 0) {
        p->hasIdle = true;
        p->idleS = n;
    } else if (strcmp(term, "after") == 0 && !p->hasAfter && n > 0) {
        p->hasAfter = true;
        p->afterS = n;
    } else if (strcmp(term, "alarm") == 0 && !p->hasAlarm && n > 0 &&
               (mode == POWER_SUSPEND || mode == POWER_SLEEP)) {
        p->hasAlarm = true;
        p->alarmS = n;
    } else if (strcmp(term, "grace") == 0 && !p->hasGrace) {
        p->hasGrace = true;
        p->graceS = n;
    } else {
        return -EINVAL;
    }
    return 0;
}

/* One policy line: a mode and its terms, split on spaces. */
static int parseLine(const power_dev_t *d, int *lastMode, policy_line_t *pol,
                     char *line) {
    char *rest = splitAt(line, ' ');
    int mode = findMode(line, strlen(line));
    if (mode <= POWER_ACTIVE || mode <= *lastMode || !modeAvailable(d, mode))
        return -EINVAL;
    *lastMode = mode;

    policy_line_t *p = &pol[mode];
    memset(p, 0, sizeof(*p));
    p->used = true;
    while (rest != NULL) {
        char *term = rest;
        rest = splitAt(rest, ' ');
        if (term[0] == '\0')
            continue;
        int rc = parseTerm(d, (unsigned)mode, p, term);
        if (rc < 0)
            return rc;
    }
    return 0;
}

/* Parse the whole text into `pol` and leave the device alone. */
static int parsePolicy(const power_dev_t *d, const char *text, size_t len,
                       policy_line_t *pol) {
    char work[POWER_POLICY_MAX];
    if (len >= sizeof(work))
        return -EINVAL;
    memcpy(work, text, len);
    work[len] = '\0';

    memset(pol, 0, sizeof(policy_line_t) * POWER_MODE_COUNT);
    int lastMode = POWER_ACTIVE;
    char *cur = work;
    while (cur != NULL) {
        char *line = cur;
        cur = splitAt(cur, '\n');
        if (line[0] == '\0')
            continue;
        int rc = parseLine(d, &lastMode, pol, line);
        if (rc < 0)
            return rc;
    }
    return 0;
}

static int applyPolicy(power_dev_t *d, const char *text, size_t len) {
    policy_line_t pol[POWER_MODE_COUNT];
    int rc = parsePolicy(d, text, len, pol);
    if (rc < 0)
        return rc;
    memcpy(d->pol, pol, sizeof(pol));
    while (len > 0 && text[len - 1] == '\n')
        len--;
    memcpy(d->policyText, text, len);
    d->policyText[len] = '\0';
    return 0;
}

/* ── Ladder ──────────────────────────────────────────────────────────────── */

/* The previous mode on the ladder before `t`: the last line used, or active. */
static unsigned ladderPrev(const power_dev_t *d, unsigned t) {
    for (unsigned m = t; m-- > POWER_STANDBY;) {
        if (d->pol[m].used)
            return m;
    }
    return POWER_ACTIVE;
}

static uint64_t graceMs(const power_dev_t *d, unsigned target) {
    const policy_line_t *p = &d->pol[target];
    return p->used && p->hasGrace ? p->graceS * POWER_MS_PER_S
                                  : POWER_GRACE_DEFAULT_MS;
}

static bool lineDue(const power_dev_t *d, unsigned t, uint64_t now, bool ext) {
    const policy_line_t *p = &d->pol[t];
    if (!p->used || (p->battery && ext))
        return false;
    if (p->hasIdle && now - d->activityMs >= p->idleS * POWER_MS_PER_S)
        return true;
    return p->hasAfter && ladderPrev(d, t) == d->mode &&
           now - d->sinceMs >= p->afterS * POWER_MS_PER_S;
}

/* Queue the transition to `target`; a deeper pending one stands. */
static void beginTransition(power_dev_t *d, unsigned target, bool forced,
                            uint64_t graceMsValue) {
    if (d->pending && !forced && d->target >= target && !d->reenter)
        return;
    d->pending = true;
    d->ready = false;
    d->forced = forced;
    d->reenter = false;
    d->target = (uint8_t)target;
    d->dueMs = nowMs() + graceMsValue;
    pushEvent(d, "enter", target, NULL);
}

static void evaluate(power_dev_t *d, uint64_t now) {
    if ((d->pending && !d->reenter) || d->mode >= POWER_OFF)
        return;
    bool ext = externalPower(d);
    for (unsigned t = d->mode + 1; t <= POWER_OFF; t++) {
        if (lineDue(d, t, now, ext)) {
            beginTransition(d, t, false, graceMs(d, t));
            return;
        }
    }
}

/* The timer a sleeping `target` arms, from the ladder and the alarm. */
static void computeArm(power_dev_t *d, unsigned target, power_arm_t *arm) {
    memset(arm, 0, sizeof(*arm));
    arm->wakeMask = d->pol[target].used ? d->pol[target].wake : 0;
    uint64_t now = nowMs();
    bool ext = externalPower(d);
    uint64_t best = 0;
    uint8_t kind = POWER_ARMED_NONE;

    for (unsigned t = target + 1; t <= POWER_OFF; t++) {
        const policy_line_t *p = &d->pol[t];
        if (!p->used || (p->battery && ext))
            continue;
        if (p->hasAfter && ladderPrev(d, t) == target) {
            uint64_t ms = p->afterS * POWER_MS_PER_S;
            if (kind == POWER_ARMED_NONE || ms < best) {
                best = ms;
                kind = POWER_ARMED_LADDER;
            }
        }
        if (p->hasIdle) {
            uint64_t due = d->activityMs + p->idleS * POWER_MS_PER_S;
            uint64_t ms = due > now ? due - now : 1;
            if (kind == POWER_ARMED_NONE || ms < best) {
                best = ms;
                kind = POWER_ARMED_LADDER;
            }
        }
    }
    uint32_t alarmS = d->alarmOverrideS;
    if (alarmS == 0 && d->pol[target].used && d->pol[target].hasAlarm)
        alarmS = d->pol[target].alarmS;
    if (alarmS != 0 &&
        (kind == POWER_ARMED_NONE || alarmS * POWER_MS_PER_S <= best)) {
        best = alarmS * POWER_MS_PER_S;
        kind = POWER_ARMED_ALARM;
    }
    d->armed = kind;
    arm->timerUs = kind == POWER_ARMED_NONE ? 0 : best * POWER_US_PER_MS;
}

/* ── Transitions ─────────────────────────────────────────────────────────── */

static const char *resumeAll(power_dev_t *d, power_cause_t cause);

/* Resume what a ladder wake left down. Caller holds the lock, which is
 * dropped across the hooks. */
static void liftHardware(power_dev_t *d) {
    if (!d->down)
        return;
    d->down = false;
    d->busy = true;
    PlatformMutexUnlock(table.lock);
    (void)resumeAll(d, POWER_CAUSE_TIMER);
    PlatformMutexLock(table.lock);
    d->busy = false;
}

/* Back toward active at once; no wapp is asked. Caller holds the lock. */
static void returnTo(power_dev_t *d, unsigned target, const char *cause) {
    liftHardware(d);
    pushEvent(d, "exit", d->mode, cause);
    d->mode = (uint8_t)target;
    d->sinceMs = nowMs();
    d->activityMs = d->sinceMs;
    d->pending = false;
}

static int requestMode(power_dev_t *d, unsigned target) {
    if (!modeAvailable(d, target))
        return -EINVAL;
    if (target == d->mode)
        return 0;
    if (target < d->mode) {
        returnTo(d, target, "request");
        return 0;
    }
    beginTransition(d, target, false, graceMs(d, target));
    return 0;
}

static const char *wakeWord(power_cause_t cause, const char *fromBoard,
                            bool alarm) {
    if (fromBoard != NULL)
        return fromBoard;
    if (cause == POWER_CAUSE_TIMER)
        return alarm ? "alarm" : "timer";
    return "key";
}

/* Reverse the sleep: the board first, then the clients. */
static const char *resumeAll(power_dev_t *d, power_cause_t cause) {
    const char *word = NULL;
    if (d->desc.ops->resume != NULL)
        word = d->desc.ops->resume(d->desc.ctx, cause);
    for (uint8_t i = 0; i < table.clientCount; i++) {
        if (table.clients[i].resume != NULL)
            table.clients[i].resume(table.clients[i].ctx);
    }
    return word;
}

static void suspendClients(void) {
    for (uint8_t i = table.clientCount; i-- > 0;) {
        if (table.clients[i].suspend != NULL)
            table.clients[i].suspend(table.clients[i].ctx);
    }
}

static power_cause_t causeOf(plat_power_wake_t wake) {
    return wake == PLAT_POWER_WAKE_TIMER ? POWER_CAUSE_TIMER
                                         : POWER_CAUSE_EXTERNAL;
}

/* A wake from `slept`. Caller holds the lock. */
static void afterWake(power_dev_t *d, unsigned slept, power_cause_t cause,
                      const char *fromBoard) {
    bool alarm = cause == POWER_CAUSE_TIMER && d->armed == POWER_ARMED_ALARM;
    setWake(d, wakeWord(cause, fromBoard, alarm));
    d->busy = false;

    if (cause == POWER_CAUSE_TIMER) {
        d->mode = (uint8_t)slept;
        if (alarm) {
            pushEvent(d, "exit", slept, "alarm");
            beginTransition(d, slept, true, graceMs(d, slept));
            d->forced = false;
            d->reenter = true;
        } else {
            d->pending = false;
        }
        return;
    }
    d->mode = (uint8_t)slept;
    unsigned resume = d->pol[slept].used ? d->pol[slept].resume : POWER_ACTIVE;
    returnTo(d, resume, d->wake);
}

static void persistLadder(const power_dev_t *d, uint64_t now) {
    persist_t p;
    memset(&p, 0, sizeof(p));
    p.magic = POWER_PERSIST_MAGIC;
    strncpy(p.name, d->name, POWER_NAME_MAX);
    p.mode = d->mode;
    p.armed = d->armed;
    p.sinceAgeS = (uint32_t)((now - d->sinceMs) / POWER_MS_PER_S);
    p.activityAgeS = (uint32_t)((now - d->activityMs) / POWER_MS_PER_S);
    memcpy(p.policyText, d->policyText, sizeof(p.policyText));
    PlatformPowerStateStore(&p, sizeof(p));
}

/* Enter a light or a deep sleep; the lock is dropped across the hooks.
 * `clientsDown` skips the client suspend a failed power-off already ran. */
static void sleepIn(power_dev_t *d, unsigned target, bool deep, bool keyOnly,
                    bool clientsDown) {
    power_arm_t arm;
    computeArm(d, target, &arm);
    arm.powerKeyOnly = keyOnly;
    if (keyOnly) {
        arm.wakeMask = 0;
        arm.timerUs = 0;
        d->armed = POWER_ARMED_NONE;
    }
    if (d->mode != target)
        d->sinceMs = nowMs();
    d->mode = (uint8_t)target;
    d->pending = false;
    d->busy = true;
    power_cause_t cause = POWER_CAUSE_TIMER;
    const char *word = NULL;
    int rc = 0;
    bool skipSuspend = clientsDown || d->down;
    d->down = false;

    PlatformMutexUnlock(table.lock);
    if (!skipSuspend)
        suspendClients();
    const power_ops_t *ops = d->desc.ops;
    if (deep && ops->sleep != NULL)
        rc = ops->sleep(d->desc.ctx, &arm);
    else if (!deep && ops->suspend != NULL)
        rc = ops->suspend(d->desc.ctx, &arm);
    if (rc >= 0) {
        if (deep) {
            PlatformMutexLock(table.lock);
            persistLadder(d, nowMs());
            PlatformMutexUnlock(table.lock);
            PlatformPowerDeepSleep(arm.timerUs);
        }
        cause = causeOf(PlatformPowerLightSleep(arm.timerUs));
    }
    /* A ladder timer wakes only to go deeper, so the hardware stays down. */
    bool stayDown =
        rc >= 0 && cause == POWER_CAUSE_TIMER && d->armed == POWER_ARMED_LADDER;
    if (!stayDown)
        word = resumeAll(d, cause);
    PlatformMutexLock(table.lock);
    if (stayDown) {
        d->down = true;
        d->downUntilMs = nowMs() + POWER_LADDER_SLACK_MS;
    }

    if (rc < 0) {
        d->busy = false;
        d->mode = POWER_ACTIVE;
        d->sinceMs = nowMs();
        pushEvent(d, "exit", target, "failed");
        return;
    }
    afterWake(d, target, cause, word);
}

/* The device could not be cut off: sleep deeply with only the power key. */
static void offFallback(power_dev_t *d, bool clientsDown) {
    d->mode = POWER_SLEEP;
    sleepIn(d, POWER_SLEEP, true, true, clientsDown);
}

static void powerOff(power_dev_t *d) {
    bool cuts = d->desc.cutsWithExternalPower;
    bool ext = false;
    PlatformMutexUnlock(table.lock);
    ext = externalPower(d);
    PlatformMutexLock(table.lock);
    d->mode = POWER_OFF;
    d->sinceMs = nowMs();
    d->pending = false;
    if ((!cuts && ext) || d->desc.ops->poweroff == NULL) {
        offFallback(d, false);
        return;
    }
    bool down = d->down;
    d->down = false;
    d->busy = true;
    PlatformMutexUnlock(table.lock);
    if (!down)
        suspendClients();
    d->desc.ops->poweroff(d->desc.ctx);
    PlatformMutexLock(table.lock);
    d->busy = false;
    offFallback(d, true);
}

/* Run the pending transition. Caller holds the lock. */
static void runPending(power_dev_t *d) {
    unsigned target = d->target;
    d->pending = false;
    d->ready = false;
    if (d->reenter)
        target = d->mode;
    d->reenter = false;

    switch (target) {
    case POWER_STANDBY:
        d->mode = POWER_STANDBY;
        d->sinceMs = nowMs();
        break;
    case POWER_SUSPEND:
        sleepIn(d, POWER_SUSPEND, false, false, false);
        break;
    case POWER_SLEEP:
        sleepIn(d, POWER_SLEEP, true, false, false);
        break;
    default:
        powerOff(d);
        break;
    }
}

void PowerTick(void) {
    if (table.lock == NULL)
        return;
    PlatformMutexLock(table.lock);
    uint64_t now = nowMs();
    for (size_t i = 0; i < CONFIG_WANTED_POWER_MAX_DEVICES; i++) {
        power_dev_t *d = &table.devs[i];
        if (!d->used || d->busy)
            continue;
        evaluate(d, now);
        if (d->down && !d->pending && now >= d->downUntilMs)
            liftHardware(d);
        if (d->pending && (d->ready || now >= d->dueMs))
            runPending(d);
    }
    PlatformMutexUnlock(table.lock);
}

int PowerNotify(const char *name, power_notice_t notice) {
    (void)notice;
    if (name == NULL || table.lock == NULL)
        return -ENOENT;
    PlatformMutexLock(table.lock);
    power_dev_t *d = findDevice(name, strlen(name));
    if (d != NULL && d->mode != POWER_OFF &&
        !(d->pending && d->forced && d->target == POWER_OFF)) {
        uint64_t grace = graceMs(d, POWER_OFF);
        if (grace > POWER_GRACE_SAFETY_MS)
            grace = POWER_GRACE_SAFETY_MS;
        d->pending = false;
        beginTransition(d, POWER_OFF, true, grace);
    }
    PlatformMutexUnlock(table.lock);
    return d != NULL ? 0 : -ENOENT;
}

/* ── Registration ────────────────────────────────────────────────────────── */

static power_dev_t *freeSlot(void) {
    for (size_t i = 0; i < CONFIG_WANTED_POWER_MAX_DEVICES; i++) {
        if (!table.devs[i].used)
            return &table.devs[i];
    }
    return NULL;
}

static bool validDesc(const power_device_desc_t *desc) {
    if (desc == NULL || desc->name == NULL ||
        !validToken(desc->name, strlen(desc->name), POWER_NAME_MAX) ||
        desc->ops == NULL || desc->wakeCount > POWER_WAKE_MAX ||
        (desc->wakeCount > 0 && desc->wakes == NULL) ||
        (desc->modes & ~(POWER_BIT(POWER_STANDBY) | POWER_BIT(POWER_SUSPEND) |
                         POWER_BIT(POWER_SLEEP))) != 0)
        return false;
    if (((desc->modes & POWER_BIT(POWER_SUSPEND)) != 0 &&
         (desc->ops->suspend == NULL || desc->ops->resume == NULL)) ||
        ((desc->modes & POWER_BIT(POWER_SLEEP)) != 0 &&
         (desc->ops->sleep == NULL || desc->ops->resume == NULL)))
        return false;
    for (unsigned i = 0; i < desc->wakeCount; i++) {
        const power_wake_desc_t *w = &desc->wakes[i];
        if (w->name == NULL ||
            !validToken(w->name, strlen(w->name), POWER_WORD_MAX))
            return false;
    }
    return true;
}

/* A deep sleep ended: restore the ladder the engine saved, and report the
 * wake. Caller holds the lock. */
static void restoreLadder(power_dev_t *d) {
    plat_power_boot_t boot = PlatformPowerBootCause();
    persist_t p;
    uint64_t elapsedUs = 0;
    if (boot == PLAT_POWER_BOOT_COLD ||
        !PlatformPowerStateLoad(&p, sizeof(p), &elapsedUs) ||
        p.magic != POWER_PERSIST_MAGIC ||
        strncmp(p.name, d->name, POWER_NAME_MAX) != 0 ||
        p.mode >= POWER_MODE_COUNT)
        return;

    p.policyText[POWER_POLICY_MAX - 1] = '\0';
    if (applyPolicy(d, p.policyText, strlen(p.policyText)) < 0)
        return;
    uint64_t now = nowMs();
    uint64_t gone = elapsedUs / POWER_US_PER_MS;
    d->sinceMs = now - (p.sinceAgeS * POWER_MS_PER_S + gone);
    d->activityMs = now - (p.activityAgeS * POWER_MS_PER_S + gone);
    d->armed = p.armed;
    d->mode = p.mode;
    power_cause_t cause = boot == PLAT_POWER_BOOT_TIMER ? POWER_CAUSE_TIMER
                                                        : POWER_CAUSE_EXTERNAL;
    const char *word = NULL;
    if (d->desc.ops->resume != NULL)
        word = d->desc.ops->resume(d->desc.ctx, cause);
    afterWake(d, p.mode, cause, word);
}

int PowerDeviceRegister(const power_device_desc_t *desc) {
    if (!validDesc(desc))
        return -EINVAL;

    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = 0;
    power_dev_t *d = NULL;
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
        strncpy(d->name, desc->name, POWER_NAME_MAX);
        d->desc = *desc;
        d->mode = POWER_ACTIVE;
        d->sinceMs = nowMs();
        d->activityMs = d->sinceMs;
        setWake(d, "cold");
        restoreLadder(d);
    }
    PlatformMutexUnlock(table.lock);
    return rc;
}

int PowerClientRegister(const power_client_t *client) {
    if (client == NULL || (client->suspend == NULL && client->resume == NULL))
        return -EINVAL;
    if (table.lock == NULL)
        table.lock = PlatformMutexNew();
    PlatformMutexLock(table.lock);
    int rc = 0;
    if (table.clientCount >= CONFIG_WANTED_POWER_MAX_CLIENTS)
        rc = -ENOSPC;
    else
        table.clients[table.clientCount++] = *client;
    PlatformMutexUnlock(table.lock);
    return rc;
}

void PowerDevicesReset(void) {
    PlatformMutexFree(table.lock);
    memset(&table, 0, sizeof(table));
}

/* ── Grant parsing ───────────────────────────────────────────────────────── */

/* Parse devices=<name>[,control]. Returns 0 or a negative errno; the caller
 * destroys the context on failure. */
static int parseGrant(struct vfs_driver_ctx_t *ctx, const char *options) {
    const size_t keyLen = sizeof(POWER_DEVICES_KEY) - 1;
    if (options == NULL || strncmp(options, POWER_DEVICES_KEY, keyLen) != 0 ||
        options[keyLen] == '\0') {
        DEBUG_TRACE("power grant has no devices= clause");
        return -EINVAL;
    }
    size_t len = strlen(options + keyLen);
    char *work = WantedMalloc(len + 1);
    if (work == NULL)
        return -ENOMEM;
    memcpy(work, options + keyLen, len + 1);

    const char *control = splitAt(work, ',');
    int rc = 0;
    if (control != NULL) {
        if (strcmp(control, POWER_CONTROL_TOKEN) == 0)
            ctx->control = true;
        else
            rc = -EINVAL;
    }
    power_dev_t *d = rc == 0 ? findDevice(work, strlen(work)) : NULL;
    if (rc == 0 &&
        (d == NULL || !validToken(work, strlen(work), POWER_NAME_MAX))) {
        DEBUG_TRACE("power grant names an unknown device");
        rc = -EINVAL;
    }
    if (rc == 0 && ctx->control) {
        if (d->controlOwned)
            rc = -EBUSY;
        else
            d->controlOwned = true;
    }
    if (rc == 0)
        ctx->devs[ctx->count++] = d;
    WantedFree(work);
    return rc;
}

/* ── Path resolution ─────────────────────────────────────────────────────── */

static const struct {
    const char *name;
    uint8_t node;
    bool control;
} nodes[] = {
    {"policy", POWER_NODE_POLICY, true}, {"mode", POWER_NODE_MODE, false},
    {"event", POWER_NODE_EVENT, false},  {"wake", POWER_NODE_WAKE, false},
    {"caps", POWER_NODE_CAPS, false},    {"rails", POWER_NODE_RAILS, false},
    {"ctl", POWER_NODE_CTL, true},
};
#define POWER_NODE_COUNT (sizeof(nodes) / sizeof(nodes[0]))

static int resolve(const struct vfs_driver_ctx_t *ctx, const char *path,
                   uint8_t *node, uint8_t *dev) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0') {
        *node = POWER_NODE_ROOT;
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
        *node = POWER_NODE_DEVICE;
        return 0;
    }
    for (size_t i = 0; i < POWER_NODE_COUNT; i++) {
        if (strcmp(slash, nodes[i].name) == 0 &&
            (!nodes[i].control || ctx->control)) {
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
static void _SetWake(vfs_driver_ctx_t d, int fd);
static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags);
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

vfs_driver_t *VfsPowerInit(const wapp_t *wapp, const char *options) {
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
    driver->SetWake = _SetWake;
    driver->SetFlags = _SetFlags;
    driver->Poll = _Poll;
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

/* The control grant frees with its wapp; the ladder stays. */
static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    if (ctx != NULL) {
        PlatformMutexLock(table.lock);
        for (uint8_t i = 0; i < ctx->count && ctx->control; i++)
            ctx->devs[i]->controlOwned = false;
        PlatformMutexUnlock(table.lock);
        WantedFree(ctx);
    }
    WantedFree(d);
    return 0;
}

/* ── FS operations ───────────────────────────────────────────────────────── */

static struct power_fd_t *fdAt(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= POWER_MAX_FDS || !d->fds[fd].used)
        return NULL;
    return &d->fds[fd];
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    uint8_t node;
    uint8_t dev;
    int rc = resolve(d, path != NULL ? path : "", &node, &dev);
    if (rc < 0)
        return rc;

    for (int i = 0; i < POWER_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
            d->fds[i].node = node;
            d->fds[i].dev = dev;
            d->fds[i].nonblock = (flags & VFS_O_NONBLOCK) != 0;
            return i;
        }
    }
    return -EMFILE;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(f, 0, sizeof(*f));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    const struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    bool dir = f->node == POWER_NODE_ROOT || f->node == POWER_NODE_DEVICE;
    s->filetype = dir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static void _SetWake(vfs_driver_ctx_t d, int fd) { d->wakeFd = fd; }

static int _SetFlags(vfs_driver_ctx_t d, int fd, vfs_oflags_t flags) {
    struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    f->nonblock = (flags & VFS_O_NONBLOCK) != 0;
    return 0;
}

/* Only event waits on anything; every other node is always ready. */
static int _Poll(vfs_driver_ctx_t d, int fd, uint32_t *avail) {
    const struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == POWER_NODE_EVENT) {
        PlatformMutexLock(table.lock);
        size_t n = eventBytes(d->devs[f->dev]);
        PlatformMutexUnlock(table.lock);
        *avail = (uint32_t)n;
        return n > 0 ? VFS_POLL_IN : 0;
    }
    return VFS_POLL_IN | VFS_POLL_OUT;
}

static size_t formatCaps(const power_dev_t *dev, char *out, size_t outLen) {
    size_t n = 0;
    n += (size_t)snprintf(out + n, outLen - n, "modes active");
    for (unsigned m = POWER_STANDBY; m < POWER_OFF; m++) {
        if ((dev->desc.modes & POWER_BIT(m)) != 0)
            n += (size_t)snprintf(out + n, outLen - n, " %s", modeNames[m]);
    }
    n += (size_t)snprintf(out + n, outLen - n, " off\n");
    for (unsigned i = 0; i < dev->desc.wakeCount; i++) {
        n += (size_t)snprintf(out + n, outLen - n, "wake %s",
                              dev->desc.wakes[i].name);
        for (unsigned m = POWER_STANDBY; m < POWER_OFF; m++) {
            if ((dev->desc.wakes[i].modes & POWER_BIT(m)) != 0)
                n += (size_t)snprintf(out + n, outLen - n, " %s", modeNames[m]);
        }
        n += (size_t)snprintf(out + n, outLen - n, "\n");
    }
    n += (size_t)snprintf(out + n, outLen - n, "cut %s\n",
                          dev->desc.cutsWithExternalPower ? "yes" : "no");
    return n < outLen ? n : outLen - 1;
}

/* Blocks until a whole line is queued; a stop ends the wait. */
static int readEvent(vfs_driver_ctx_t d, const struct power_fd_t *f, void *buf,
                     size_t nbyte) {
    if (nbyte < POWER_EVENT_MAX)
        return -EINVAL;
    for (;;) {
        PlatformMutexLock(table.lock);
        size_t n = popEvents(d->devs[f->dev], buf, nbyte);
        PlatformMutexUnlock(table.lock);
        if (n > 0)
            return (int)n;
        if (f->nonblock)
            return -EAGAIN;
        if (PlatformClockNanoSleep(PLAT_CLOCKID_MONOTONIC, POWER_SLEEP_NS, 0) ==
                -EINTR ||
            PlatformWakeRaised(d->wakeFd))
            return -EINTR;
    }
}

/* One text block, then EOF on this descriptor. Each open reads afresh. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == POWER_NODE_ROOT || f->node == POWER_NODE_DEVICE)
        return -EISDIR;
    if (f->node == POWER_NODE_CTL)
        return -EPERM;
    if (f->node == POWER_NODE_EVENT)
        return readEvent(d, f, buf, nbyte);
    if (f->done)
        return 0;

    power_dev_t *dev = d->devs[f->dev];
    char text[POWER_TEXT_MAX];
    size_t len = 0;
    text[0] = '\0';
    PlatformMutexLock(table.lock);
    switch (f->node) {
    case POWER_NODE_POLICY:
        len = (size_t)snprintf(text, sizeof(text), "%s%s", dev->policyText,
                               dev->policyText[0] != '\0' ? "\n" : "");
        break;
    case POWER_NODE_MODE:
        len = (size_t)snprintf(
            text, sizeof(text), "%s %lu\n", modeNames[dev->mode],
            (unsigned long)((nowMs() - dev->sinceMs) / POWER_MS_PER_S));
        break;
    case POWER_NODE_WAKE:
        len = (size_t)snprintf(text, sizeof(text), "%s\n", dev->wake);
        break;
    case POWER_NODE_CAPS:
        len = formatCaps(dev, text, sizeof(text));
        break;
    default:
        if (dev->desc.ops->rails != NULL)
            len = dev->desc.ops->rails(dev->desc.ctx, text, sizeof(text));
        break;
    }
    PlatformMutexUnlock(table.lock);

    if (len >= sizeof(text))
        len = sizeof(text) - 1;
    size_t n = nbyte < len ? nbyte : len;
    memcpy(buf, text, n);
    f->done = true;
    return (int)n;
}

/* activity | ready | mode <name> | alarm <s>. Caller holds the lock. */
static int writeCtl(power_dev_t *dev, char *line) {
    const char *arg = splitAt(line, ' ');
    if (strcmp(line, "activity") == 0 && arg == NULL) {
        dev->activityMs = nowMs();
        if (dev->mode == POWER_STANDBY)
            returnTo(dev, POWER_ACTIVE, "activity");
        return 0;
    }
    if (strcmp(line, "ready") == 0 && arg == NULL) {
        if (dev->pending)
            dev->ready = true;
        return 0;
    }
    if (arg == NULL)
        return -EINVAL;
    if (strcmp(line, "mode") == 0) {
        int m = findMode(arg, strlen(arg));
        return m < 0 ? -EINVAL : requestMode(dev, (unsigned)m);
    }
    uint32_t n;
    if (strcmp(line, "alarm") == 0 && parseNumber(arg, strlen(arg), &n) == 0) {
        dev->alarmOverrideS = n;
        return 0;
    }
    return -EINVAL;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    const struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;
    if (f->node == POWER_NODE_ROOT || f->node == POWER_NODE_DEVICE)
        return -EISDIR;
    if (f->node != POWER_NODE_POLICY && f->node != POWER_NODE_CTL)
        return -EPERM;

    if (nbyte == 0 || nbyte >= POWER_POLICY_MAX)
        return -EINVAL;
    char text[POWER_POLICY_MAX];
    memcpy(text, buf, nbyte);
    text[nbyte] = '\0';

    PlatformMutexLock(table.lock);
    power_dev_t *dev = d->devs[f->dev];
    int rc;
    if (f->node == POWER_NODE_POLICY) {
        rc = applyPolicy(dev, text, nbyte);
    } else {
        if (text[nbyte - 1] == '\n')
            text[nbyte - 1] = '\0';
        rc = writeCtl(dev, text);
    }
    PlatformMutexUnlock(table.lock);
    return rc < 0 ? rc : (int)nbyte;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    const struct power_fd_t *f = fdAt(d, fd);
    if (f == NULL)
        return -EBADF;

    if (f->node == POWER_NODE_DEVICE) {
        vfs_dir_entry_t attrs[POWER_NODE_COUNT];
        size_t n = 0;
        for (size_t i = 0; i < POWER_NODE_COUNT; i++) {
            if (nodes[i].control && !d->control)
                continue;
            attrs[n].name = nodes[i].name;
            attrs[n++].type = VFS_FILETYPE_CHARACTER_DEVICE;
        }
        return VfsFlatDirReadDir(attrs, n, buf, bufLen, cookie, bufUsed);
    }
    if (f->node != POWER_NODE_ROOT)
        return -ENOTDIR;

    vfs_dir_entry_t entries[CONFIG_WANTED_POWER_MAX_DEVICES];
    for (uint8_t i = 0; i < d->count; i++) {
        entries[i].name = d->devs[i]->name;
        entries[i].type = VFS_FILETYPE_DIRECTORY;
    }
    return VfsFlatDirReadDir(entries, d->count, buf, bufLen, cookie, bufUsed);
}
