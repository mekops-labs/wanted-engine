/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cwalk.h>
#include <log-store.h>
#include <vfs-drivers.h>
#include <vfs.h>

#include <platform.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

/* ── WantedWappsDriver ───────────────────────────────────────────────────────
 *
 * The per-wapp control namespace mounted at /dev/wanted/wapps. The path
 * carries wapp identity, reads are composable plain text, and only the start
 * config is JSON.
 *
 *   wapps/                 ReadDir → one entry per known (running) wapp
 *     <name>/              synthetic dir; ReadDir → the control files below
 *       ctl       (w)      line verb: "start [<image>]" | "stop" (identity=path)
 *       state     (r)      plain-text token: not_started|starting|running|...
 *       image     (r)      plain-text registry image the instance runs
 *       version   (r)      plain-text version, e.g. "1.0.0-0"
 *       id        (r)      plain-text engine wapp id
 *       exit_code (r)      plain-text WASI exit code (authoritative when exited)
 *       config    (w)      JSON start config: { console, drivers[], preopens,
 *                          args[], envs[] }
 *       log       (r)      ring-buffered stdout/stderr captured by the log driver
 *
 * Every open allocates its own slot in the fd table below; read EOF is latched
 * per-fd, so concurrent readers (the supervisor reopens on every tick) keep
 * independent cursors.
 * ───────────────────────────────────────────────────────────────────────── */

#define ID                                                                     \
    { 'W', 'a', 'p', 'p' }

/* Longest control verb / read token we emit; bounds the write line buffer so
 * a write() never drives an unbounded (VLA) stack allocation. The widest root
 * verbs are "create <name>" / "delete <name>" (name ≤ WAPP_MAX_NAME_LEN), which
 * fit comfortably. */
#define WAPPS_LINE_MAX 32

typedef enum {
    NODE_NONE = 0,
    NODE_ROOT,    /* wapps/            */
    NODE_WAPP,    /* wapps/<name>/     */
    NODE_CTL,     /* wapps/<name>/ctl     */
    NODE_STATE,   /* wapps/<name>/state   */
    NODE_IMAGE,   /* wapps/<name>/image   */
    NODE_VERSION, /* wapps/<name>/version */
    NODE_ID,      /* wapps/<name>/id      */
    NODE_EXIT_CODE, /* wapps/<name>/exit_code */
    NODE_CONFIG,  /* wapps/<name>/config  */
    NODE_LOG,     /* wapps/<name>/log     */
} wapp_node_t;

#define WAPPS_MAX_OPEN 8

typedef struct {
    bool in_use;
    wapp_node_t node;
    char name[WAPP_MAX_NAME_LEN];
    bool read_done; /* per-fd EOF latch */
} wapps_fd_t;

/* Config written to wapps/<name>/config is buffered here until the matching
 * "start" verb is written to wapps/<name>/ctl. Bounded by MAX_WAPPS — the same
 * ceiling the runtime imposes on live wapps. `configured` distinguishes a bare
 * `create` reservation (slot exists, no config yet → state `created`) from one
 * whose config has been written (→ state `not_started`, ready to start). */
typedef struct {
    bool valid;
    bool configured;
    char name[WAPP_MAX_NAME_LEN];
    wapp_config_t cfg;
} wapps_pending_t;

static struct vfs_driver_ctx_t {
    wapps_fd_t fds[WAPPS_MAX_OPEN];
    wapps_pending_t pending[MAX_WAPPS];
} ctx;

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

const vfs_driver_t WantedWappsDriver = {
    .id = ID,
    .filetype = VFS_FILETYPE_DIRECTORY,
    .ctx = &ctx,
    .Destroy = _Destroy,
    .Open = _Open,
    .Close = _Close,
    .Stat = _Stat,
    .Read = _Read,
    .Write = _Write,
    .ReadDir = _ReadDir,
};

/* The control files exposed under each wapps/<name>/ directory. */
static const struct {
    const char *name;
    wapp_node_t node;
} LEAVES[] = {
    {"ctl", NODE_CTL},       {"state", NODE_STATE},
    {"image", NODE_IMAGE},   {"version", NODE_VERSION},
    {"id", NODE_ID},         {"exit_code", NODE_EXIT_CODE},
    {"config", NODE_CONFIG}, {"log", NODE_LOG},
};
#define N_LEAVES (sizeof(LEAVES) / sizeof(LEAVES[0]))

/* ── Path resolution ────────────────────────────────────────────────────── */

/* Parse a path relative to the wapps/ root into (node kind, wapp name).
 * Accepts "", "/", "<name>", "<name>/", "<name>/<leaf>". Returns 0 on success
 * or -ENOENT for an unknown leaf / over-deep path. */
static int ResolvePath(const char *path, wapp_node_t *node, char *name) {
    struct cwk_segment seg;

    name[0] = '\0';
    *node = NODE_NONE;

    if (path == NULL || path[0] == '\0' ||
        (path[0] == '/' && path[1] == '\0')) {
        *node = NODE_ROOT;
        return 0;
    }

    if (!cwk_path_get_first_segment(path, &seg)) {
        *node = NODE_ROOT;
        return 0;
    }

    if (seg.size >= WAPP_MAX_NAME_LEN)
        return -ENAMETOOLONG;
    memcpy(name, seg.begin, seg.size);
    name[seg.size] = '\0';

    if (!cwk_path_get_next_segment(&seg)) {
        *node = NODE_WAPP;
        return 0;
    }

    for (size_t i = 0; i < N_LEAVES; i++) {
        if (seg.size == strlen(LEAVES[i].name) &&
            strncmp(seg.begin, LEAVES[i].name, seg.size) == 0) {
            /* reject a trailing segment after a leaf (leaf/<x>) */
            if (cwk_path_get_next_segment(&seg))
                return -ENOENT;
            *node = LEAVES[i].node;
            return 0;
        }
    }
    return -ENOENT;
}

static int alloc_fd(void) {
    for (int i = 0; i < WAPPS_MAX_OPEN; i++) {
        if (!ctx.fds[i].in_use)
            return i;
    }
    return -EMFILE;
}

static wapps_pending_t *pending_find(struct vfs_driver_ctx_t *d,
                                     const char *name) {
    for (int i = 0; i < MAX_WAPPS; i++) {
        if (d->pending[i].valid &&
            strncmp(d->pending[i].name, name, WAPP_MAX_NAME_LEN) == 0)
            return &d->pending[i];
    }
    return NULL;
}

static wapps_pending_t *pending_slot(struct vfs_driver_ctx_t *d,
                                     const char *name) {
    wapps_pending_t *p = pending_find(d, name);
    if (p != NULL)
        return p;
    for (int i = 0; i < MAX_WAPPS; i++) {
        if (!d->pending[i].valid)
            return &d->pending[i];
    }
    return NULL;
}

/* ── Lifecycle helpers ──────────────────────────────────────────────────── */

/* Look up a wapp's runtime state by name. Returns true and fills *out if the
 * platform reports a slot for it; false otherwise (caller treats absence as
 * not_started). */
static bool LookupState(const char *name, wapp_state_t *out) {
    wapp_state_t states[MAX_WAPPS];
    int n = PlatformWappGetState(states, MAX_WAPPS);
    for (int i = 0; i < n; i++) {
        if (strncmp(states[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            *out = states[i];
            return true;
        }
    }
    return false;
}

/* Create-and-launch a wapp by name, applying a buffered config if present.
 * Instance identity comes from `name` (supplied by the path). The image the
 * instance runs is resolved in priority order: an explicit `image` argument
 * (from `start <image>`), else the buffered config's `image` field, else the
 * instance name. The launch config — console, argv[1..] (`args`), the
 * environment (`envs`) — is whatever was buffered at wapps/<name>/config. */
static int StartWapp(struct vfs_driver_ctx_t *d, const char *name,
                     const char *image) {
    int ret;
    reg_entry_t e;
    bool haveImage = (image != NULL && image[0] != '\0');

    /* A wapp reserved via `create` must name an image before it can start —
     * either by a prior config write or by an explicit `start <image>`. A bare
     * `create` followed by a bare `start` (neither) cannot transition straight
     * to starting and is rejected. A name with no reservation still starts with
     * defaults, as before. */
    wapps_pending_t *pend = pending_find(d, name);
    if (pend != NULL && !pend->configured && !haveImage)
        return -EINVAL;

    wapp_t *wapp = WantedMalloc(sizeof(wapp_t));
    if (NULL == wapp)
        return -ENOMEM;

    memset(wapp, 0, sizeof(*wapp));
    strncpy(wapp->name, name, WAPP_MAX_NAME_LEN - 1);

    /* The launch attempt owns the reservation: copy its config onto the wapp and
     * clear the slot now, so a failed load/start frees the pending pool and a
     * later start never silently reuses stale config. (A bare `created` slot is
     * rejected by the gate above and is left intact for a later config+start.) */
    if (pend != NULL) {
        wapp->cfg = pend->cfg;
        memset(pend, 0, sizeof(*pend));
    }

    /* Instance identity (wapp->name) is the path-supplied `name`. The image it
     * runs is the explicit `start <image>` argument, else the config's `image`,
     * else the instance name — so an unconfigured single-instance wapp runs its
     * like-named image, while many instances can share one image. The loader
     * resolves the image's registry entry and stamps image identity (image +
     * version) onto the wapp; it never touches wapp->name. */
    const char *img = haveImage ? image
                                : (wapp->cfg.image[0] ? wapp->cfg.image : name);
    memset(&e, 0, sizeof(e));
    strncpy(e.name, img, WAPP_MAX_NAME_LEN - 1);
    e.version[0] = '\0';

    ret = PlatformRegistryWappLoad(&e, wapp);
    if (ret < 0)
        goto FREE; /* nothing mapped yet */

    ret = PlatformWappStart(wapp);
    if (ret < 0)
        goto UNLOAD;

    /* Ownership of `wapp` and its mapped image transfers to the platform
     * thread slot; the worker thread dereferences it for the wapp's whole
     * lifetime. The slot releases it when later reused (see PlatformWappStart).
     * Freeing here would be a use-after-free against the just-spawned thread. */
    return ret;

UNLOAD:
    PlatformWappUnload(wapp);
FREE:
    WantedFree(wapp);
    return ret;
}

/* ── Driver ops ─────────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d) {
    memset(d->ctx, 0, sizeof(ctx));
    return 0;
}

/* True if the engine knows this wapp name — it is live (a platform slot) or has
 * a `create` reservation. The per-wapp namespace exists only for a known wapp. */
static bool WappKnown(struct vfs_driver_ctx_t *d, const char *name) {
    wapp_state_t st;
    return LookupState(name, &st) || pending_find(d, name) != NULL;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;
    wapp_node_t node;
    char name[WAPP_MAX_NAME_LEN];

    int ret = ResolvePath(path, &node, name);
    if (ret < 0)
        return ret;

    /* A wapp's directory and every node under it exist only once the wapp has
     * been brought into being by `create` (or is already live). Opening any of
     * them for an unknown name returns ENOENT rather than synthesising a default
     * — a name cannot be probed by guessing its path, and config/start are
     * reachable only from a directory that `create` actually made. */
    if (node != NODE_ROOT && !WappKnown(d, name))
        return -ENOENT;

    int fd = alloc_fd();
    if (fd < 0)
        return fd;

    d->fds[fd].in_use = true;
    d->fds[fd].node = node;
    d->fds[fd].read_done = false;
    strncpy(d->fds[fd].name, name, WAPP_MAX_NAME_LEN - 1);
    d->fds[fd].name[WAPP_MAX_NAME_LEN - 1] = '\0';

    return fd;
}

static int _Close(vfs_driver_ctx_t d, int fd) {
    if (fd < 0 || fd >= WAPPS_MAX_OPEN)
        return -EBADF;
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    if (fd < 0 || fd >= WAPPS_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;

    bool isDir =
        d->fds[fd].node == NODE_ROOT || d->fds[fd].node == NODE_WAPP;

    stat->dev = WantedWappsDriver.bytesId;
    stat->ino = 0;
    stat->filetype = isDir ? VFS_FILETYPE_DIRECTORY
                           : VFS_FILETYPE_CHARACTER_DEVICE;
    stat->nlink = 0;
    stat->size = 0;
    stat->atim = 0;
    stat->ctim = 0;
    stat->mtim = 0;
    stat->oflags = 0;
    return 0;
}

/* Render the plain-text body of a read node into `out` (NUL-terminated within
 * cap). Returns the byte length (excluding NUL). */
static size_t RenderRead(wapp_node_t node, const char *name, char *out,
                         size_t cap) {
    wapp_state_t st;
    bool live = LookupState(name, &st);

    switch (node) {
    case NODE_STATE: {
        /* For a wapp the platform doesn't run yet, the reservation distinguishes
         * a bare `create` (slot exists, no config → created) from one whose
         * config has been written (→ not_started, ready to start). An unknown
         * name (no reservation) is the not_started default. */
        status_t s;
        if (live) {
            s = st.status;
        } else {
            wapps_pending_t *p = pending_find(&ctx, name);
            s = (p != NULL && !p->configured) ? CREATED : NOT_STARTED;
        }
        return (size_t)snprintf(out, cap, "%s", statusToString(s));
    }
    case NODE_IMAGE:
        /* The registry image the instance runs. Known only once the platform has
         * launched it (the loader stamps it); a created/not-started reservation
         * has not bound an image yet, so it reads empty. */
        return (size_t)snprintf(out, cap, "%s", live ? st.image : "");
    case NODE_VERSION: {
        const uint8_t *v = live ? st.version.v : (const uint8_t[]){0, 0, 0, 0};
        return (size_t)snprintf(out, cap, "%X.%X.%X-%X", v[0], v[1], v[2],
                                v[3]);
    }
    case NODE_ID:
        return (size_t)snprintf(out, cap, "%u", live ? st.id : 0);
    case NODE_EXIT_CODE:
        /* Authoritative only when state==exited; a running/unknown wapp reads
         * the sentinel, and so does a trapped one (which never set a code). */
        return (size_t)snprintf(out, cap, "%d",
                                live ? st.exit_code : WAPP_EXIT_CODE_NONE);
    default:
        return 0;
    }
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (fd < 0 || fd >= WAPPS_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;

    wapp_node_t node = d->fds[fd].node;
    if (node == NODE_ROOT || node == NODE_WAPP)
        return -EISDIR;
    if (node == NODE_CTL || node == NODE_CONFIG)
        return -EINVAL; /* write-only nodes */

    if (d->fds[fd].read_done)
        return 0; /* EOF, latched per-fd */

    /* The log can be larger than a control line, so read it straight from the
     * log store into the caller's buffer rather than via the line buffer. */
    if (node == NODE_LOG) {
        size_t n = LogStoreRead(LogStore(), d->fds[fd].name, (char *)buf, nbyte);
        d->fds[fd].read_done = true;
        return (int)n;
    }

    char line[WAPPS_LINE_MAX];
    size_t n = RenderRead(node, d->fds[fd].name, line, sizeof(line));
    if (n >= sizeof(line))
        n = sizeof(line) - 1;
    if (n > nbyte)
        n = nbyte;
    memcpy(buf, line, n);
    d->fds[fd].read_done = true;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (buf == NULL)
        return -EINVAL;
    if (fd < 0 || fd >= WAPPS_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;

    wapp_node_t node = d->fds[fd].node;
    const char *name = d->fds[fd].name;

    if (node == NODE_CTL) {
        /* Line-oriented verb. A control line is a short token; anything that
         * fills the fixed line buffer is rejected. */
        if (nbyte >= WAPPS_LINE_MAX)
            return -EMSGSIZE;
        char line[WAPPS_LINE_MAX];
        memcpy(line, buf, nbyte);
        line[nbyte] = '\0';
        /* trim trailing whitespace/newline */
        size_t end = nbyte;
        while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r' ||
                           line[end - 1] == ' ' || line[end - 1] == '\t'))
            line[--end] = '\0';

        int ret;
        /* "start" optionally followed by an image: "start" | "start <image>".
         * The image overrides config.image; a bare "start" falls back to it. */
        if (strncmp(line, "start", 5) == 0 &&
            (line[5] == '\0' || line[5] == ' ' || line[5] == '\t')) {
            const char *img = line + 5;
            while (*img == ' ' || *img == '\t')
                img++;
            if (*img != '\0' && strlen(img) >= WAPP_MAX_NAME_LEN)
                return -EINVAL;
            ret = StartWapp(d, name, img[0] ? img : NULL);
        } else if (strcmp(line, "stop") == 0) {
            ret = PlatformWappStop(name);
        } else {
            return -EINVAL;
        }
        return ret < 0 ? ret : (int)nbyte;
    }

    if (node == NODE_CONFIG) {
        wapps_pending_t *p = pending_slot(d, name);
        if (p == NULL)
            return -ENOSPC;
        int ret = WantedParseWappConfigJson(buf, nbyte, &p->cfg);
        if (ret < 0)
            return ret;
        p->valid = true;
        p->configured = true; /* config applied → state not_started */
        strncpy(p->name, name, WAPP_MAX_NAME_LEN - 1);
        p->name[WAPP_MAX_NAME_LEN - 1] = '\0';
        return (int)nbyte;
    }

    if (node == NODE_ROOT || node == NODE_WAPP)
        return -EISDIR;

    return -EINVAL; /* read-only nodes (state/version/id) */
}

/* Pack a single dirent + name into buf at *used, advancing it. Returns false
 * if it would overflow bufLen (caller stops and reports partial fill). */
static bool PackDirent(void *buf, size_t bufLen, size_t *used, uint64_t ino,
                       uint64_t next, vfs_filetype_t type, const char *name,
                       size_t nameLen) {
    vfs_dirent_t dir;
    dir.d_ino = ino;
    dir.d_namlen = (uint32_t)nameLen;
    dir.d_type = type;
    dir.d_next = next;

    if (*used + sizeof(dir) + nameLen > bufLen)
        return false;
    memcpy((char *)buf + *used, &dir, sizeof(dir));
    *used += sizeof(dir);
    memcpy((char *)buf + *used, name, nameLen);
    *used += nameLen;
    return true;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (buf == NULL || cookie == NULL || bufUsed == NULL)
        return -EINVAL;
    if (fd < 0 || fd >= WAPPS_MAX_OPEN || !d->fds[fd].in_use)
        return -EBADF;

    wapp_node_t node = d->fds[fd].node;
    size_t used = 0;

    if (node == NODE_ROOT) {
        wapp_state_t states[MAX_WAPPS];
        int n = PlatformWappGetState(states, MAX_WAPPS);
        if (n < 0)
            return n;

        /* Enumerate the live wapps plus any `create`d-but-not-running ones
         * (reserved pending slots), de-duplicated by name, into one list so the
         * cookie can page over a stable index space. */
        char names[2 * MAX_WAPPS][WAPP_MAX_NAME_LEN];
        int total = 0;
        for (int k = 0; k < n; k++) {
            strncpy(names[total], states[k].name, WAPP_MAX_NAME_LEN - 1);
            names[total][WAPP_MAX_NAME_LEN - 1] = '\0';
            total++;
        }
        for (int k = 0; k < MAX_WAPPS; k++) {
            if (!d->pending[k].valid)
                continue;
            bool dup = false;
            for (int j = 0; j < n; j++) {
                if (strncmp(d->pending[k].name, states[j].name,
                            WAPP_MAX_NAME_LEN) == 0) {
                    dup = true;
                    break;
                }
            }
            if (dup)
                continue;
            strncpy(names[total], d->pending[k].name, WAPP_MAX_NAME_LEN - 1);
            names[total][WAPP_MAX_NAME_LEN - 1] = '\0';
            total++;
        }

        uint64_t i = *cookie;
        for (; i < (uint64_t)total; i++) {
            size_t nameLen = strnlen(names[i], WAPP_MAX_NAME_LEN);
            if (!PackDirent(buf, bufLen, &used, i, i + 1,
                            VFS_FILETYPE_DIRECTORY, names[i], nameLen))
                break;
        }
        *cookie = i;
        *bufUsed = used;
        return 0;
    }

    if (node == NODE_WAPP) {
        uint64_t i = *cookie;
        for (; i < N_LEAVES; i++) {
            if (!PackDirent(buf, bufLen, &used, i, i + 1,
                            VFS_FILETYPE_CHARACTER_DEVICE, LEAVES[i].name,
                            strlen(LEAVES[i].name)))
                break;
        }
        *cookie = i;
        *bufUsed = used;
        return 0;
    }

    return -ENOTDIR;
}

/* ── WantedCtlDriver ─────────────────────────────────────────────────────────
 *
 * The root /dev/wanted/ctl node. Write-only engine command channel:
 *
 *   write /dev/wanted/ctl create <name>   register a per-wapp namespace
 *   write /dev/wanted/ctl delete <name>   release a wapp slot (→ -ENOENT again)
 *   write /dev/wanted/ctl poweroff        stop the engine (no respawn)
 *   write /dev/wanted/ctl reboot          restart the engine / reset the board
 *
 * (wsh has no shell redirection; its `write` builtin joins its trailing tokens
 * with single spaces and writes them to the node in one write().)
 *
 * The root ctl does not launch wapps — a wapp is started through its own
 * wapps/<name>/ctl after `create` and an optional config write. `create`
 * reserves the namespace and `delete` frees it (identity travels in the verb
 * argument, not a JSON payload); poweroff/reboot take no argument. This node is
 * the supervisor's
 * capability: it is reachable only by a wapp whose launch grants the
 * /dev/wanted driver, so an ordinary wapp cannot issue these commands.
 * ───────────────────────────────────────────────────────────────────────── */

#define CTL_ID                                                                 \
    { 'W', 'c', 't', 'l' }

static int _ctl_Destroy(struct vfs_driver_t *d) {
    (void)d;
    return 0;
}

static int _ctl_Open(vfs_driver_ctx_t d, const char *path,
                     vfs_oflags_t flags) {
    (void)d;
    (void)path;
    (void)flags;
    return 0;
}

static int _ctl_Close(vfs_driver_ctx_t d, int fd) {
    (void)d;
    (void)fd;
    return 0;
}

static int _ctl_Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat) {
    (void)d;
    (void)fd;
    if (stat == NULL)
        return -EINVAL;
    memset(stat, 0, sizeof(*stat));
    stat->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

static int _ctl_Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    (void)d;
    (void)fd;
    (void)buf;
    (void)nbyte;
    return -EINVAL; /* write-only */
}

static int _ctl_Write(vfs_driver_ctx_t d, int fd, const void *buf,
                      size_t nbyte) {
    (void)d;
    (void)fd;
    if (buf == NULL)
        return -EINVAL;
    if (nbyte >= WAPPS_LINE_MAX)
        return -EMSGSIZE;

    char line[WAPPS_LINE_MAX];
    memcpy(line, buf, nbyte);
    line[nbyte] = '\0';
    size_t end = nbyte;
    while (end > 0 && (line[end - 1] == '\n' || line[end - 1] == '\r' ||
                       line[end - 1] == ' ' || line[end - 1] == '\t'))
        line[--end] = '\0';

    /* System control: no argument. The engine respawns a vanished supervisor
     * forever, so these are the only paths that end the run loop. Match the
     * whole token (sizeof includes the NUL) so the bounded line buffer is never
     * read past its terminator. */
    if (strncmp(line, "poweroff", sizeof("poweroff")) == 0) {
        PlatformRequestShutdown();
        return (int)nbyte;
    }
    if (strncmp(line, "reboot", sizeof("reboot")) == 0) {
        PlatformRequestReboot();
        return (int)nbyte;
    }

    /* "create <name>": register the per-wapp namespace ahead of a config write
     * and start, without launching anything. Reserves a pending slot (zeroed
     * config) so wapps/<name>/ enumerates and a later config write targets it. */
    static const char CREATE_VERB[] = "create ";
    size_t clen = sizeof(CREATE_VERB) - 1;
    if (strncmp(line, CREATE_VERB, clen) == 0) {
        const char *cname = line + clen;
        while (*cname == ' ' || *cname == '\t')
            cname++;
        if (*cname == '\0' || strlen(cname) >= WAPP_MAX_NAME_LEN)
            return -EINVAL;
        wapps_pending_t *p = pending_slot(&ctx, cname);
        if (p == NULL)
            return -ENOSPC;
        if (!p->valid) {
            memset(&p->cfg, 0, sizeof(p->cfg));
            strncpy(p->name, cname, WAPP_MAX_NAME_LEN - 1);
            p->name[WAPP_MAX_NAME_LEN - 1] = '\0';
            p->valid = true;
            p->configured = false; /* bare reservation → state created */
        }
        return (int)nbyte;
    }

    /* "delete <name>": release a wapp slot so the name leaves wapps/ and its
     * nodes return -ENOENT again. Frees a `create` reservation and/or a terminal
     * (exited/failure) platform slot. A running wapp is rejected with -EBUSY —
     * it must be stopped first (no implicit stop-then-delete). An unknown name
     * (no reservation, no platform slot) is -ENOENT. */
    static const char DELETE_VERB[] = "delete ";
    size_t dlen = sizeof(DELETE_VERB) - 1;
    if (strncmp(line, DELETE_VERB, dlen) == 0) {
        const char *dname = line + dlen;
        while (*dname == ' ' || *dname == '\t')
            dname++;
        if (*dname == '\0' || strlen(dname) >= WAPP_MAX_NAME_LEN)
            return -EINVAL;

        wapp_state_t st;
        bool live = LookupState(dname, &st);
        wapps_pending_t *p = pending_find(&ctx, dname);
        if (!live && p == NULL)
            return -ENOENT;

        /* Release the platform slot first: a running wapp returns -EBUSY, which
         * leaves the reservation (if any) intact so the caller can retry after a
         * stop rather than losing buffered config to a half-applied delete. */
        if (live) {
            int ret = PlatformWappRelease(dname);
            if (ret < 0)
                return ret;
        }
        if (p != NULL)
            memset(p, 0, sizeof(*p));
        return (int)nbyte;
    }

    /* The root ctl does not launch wapps: a wapp is started through its own
     * wapps/<name>/ctl after `create` (and an optional config write). Any verb
     * other than create/delete/poweroff/reboot is rejected. */
    return -EINVAL;
}

const vfs_driver_t WantedCtlDriver = {
    .id = CTL_ID,
    .filetype = VFS_FILETYPE_CHARACTER_DEVICE,
    .Destroy = _ctl_Destroy,
    .Open = _ctl_Open,
    .Close = _ctl_Close,
    .Stat = _ctl_Stat,
    .Read = _ctl_Read,
    .Write = _ctl_Write,
};
