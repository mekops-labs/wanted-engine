/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <cwalk.h>
#include <vfs-drivers.h>
#include <vfs.h>

#include <platform.h>
#include <wanted-api.h>
#include <wanted-vfs-api.h>
#include <wanted_malloc.h>

/* WantedWappsDriver — the per-wapp control namespace at /dev/wanted/wapps. The
 * path carries wapp identity; reads are plain text and only the start config is
 * JSON. Every open gets its own fd slot, so readers keep separate cursors. */

#define ID {'W', 'a', 'p', 'p'}

/* Longest control verb / read token emitted; bounds the write line buffer so a
 * write() never drives an unbounded stack allocation. The widest root verbs are
 * "create <name>" and "delete <name>". */
#define WAPPS_LINE_MAX 32

typedef enum {
    NODE_NONE = 0,
    NODE_ROOT,   /* wapps/             */
    NODE_WAPP,   /* wapps/<name>/      */
    NODE_CTL,    /* wapps/<name>/ctl   */
    NODE_STATE,  /* wapps/<name>/state */
    NODE_CONFIG, /* wapps/<name>/config */
} wapp_node_t;

#define WAPPS_MAX_OPEN 8

typedef struct {
    bool in_use;
    wapp_node_t node;
    char name[WAPP_MAX_NAME_LEN];
    bool read_done; /* per-fd EOF latch */
} wapps_fd_t;

/* Config written to wapps/<name>/config is buffered here until the matching
 * "start". Bounded by CONFIG_WANTED_MAX_WAPPS. `configured` separates a bare
 * `create` reservation from one whose config has been written. */
typedef struct {
    bool valid;
    bool configured;
    char name[WAPP_MAX_NAME_LEN];
    wapp_config_t cfg;
} wapps_pending_t;

static struct vfs_driver_ctx_t {
    wapps_fd_t fds[WAPPS_MAX_OPEN];
    /* CONFIG_WANTED_MAX_WAPPS launch-config slots, heap-allocated on first
     * `create`. wapp_config_t is large, so this stays off static .bss to spare
     * internal RAM; the engine heap may extend into PSRAM. */
    wapps_pending_t *pending;
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
    {"ctl", NODE_CTL},
    {"state", NODE_STATE},
    {"config", NODE_CONFIG},
};
#define N_LEAVES (sizeof(LEAVES) / sizeof(LEAVES[0]))

/* ── Path resolution ────────────────────────────────────────────────────── */

/* Parse a path relative to the wapps/ root into (node kind, wapp name).
 * Accepts "", "/", "<name>", "<name>/", "<name>/<leaf>". Returns 0 on success
 * or -ENOENT for an unknown leaf / over-deep path. */
static int resolvePath(const char *path, wapp_node_t *node, char *name) {
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
    if (d->pending == NULL)
        return NULL;
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
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
    if (d->pending == NULL) {
        d->pending =
            WantedMalloc(CONFIG_WANTED_MAX_WAPPS * sizeof(wapps_pending_t));
        if (d->pending == NULL)
            return NULL;
        memset(d->pending, 0,
               CONFIG_WANTED_MAX_WAPPS * sizeof(wapps_pending_t));
    }
    for (int i = 0; i < CONFIG_WANTED_MAX_WAPPS; i++) {
        if (!d->pending[i].valid)
            return &d->pending[i];
    }
    return NULL;
}

/* ── Lifecycle helpers ──────────────────────────────────────────────────── */

/* Look up a wapp's runtime state by name. Returns true and fills *out if the
 * platform reports a slot for it; false otherwise (caller treats absence as
 * not_started). */
static bool lookupState(const char *name, wapp_state_t *out) {
    wapp_state_t states[CONFIG_WANTED_MAX_WAPPS];
    int n = PlatformWappGetState(states, CONFIG_WANTED_MAX_WAPPS);
    for (int i = 0; i < n; i++) {
        if (strncmp(states[i].name, name, WAPP_MAX_NAME_LEN) == 0) {
            *out = states[i];
            return true;
        }
    }
    return false;
}

/* Create-and-launch a wapp by name, applying a buffered config if present.
 * Instance identity comes from `name`; the image is the explicit `start`
 * argument, else the buffered config's `image`, else the instance name. */
static int startWapp(struct vfs_driver_ctx_t *d, const char *name,
                     const char *image) {
    int ret;
    reg_entry_t e;
    bool haveImage = (image != NULL && image[0] != '\0');

    /* A wapp reserved via `create` must name an image before it can start,
     * through a config write or an explicit `start <image>`; a bare `create`
     * then a bare `start` is rejected. An unreserved name still runs. */
    wapps_pending_t *pend = pending_find(d, name);
    if (pend != NULL && !pend->configured && !haveImage)
        return -EINVAL;

    wapp_t *wapp = WantedMalloc(sizeof(wapp_t));
    if (NULL == wapp)
        return -ENOMEM;

    memset(wapp, 0, sizeof(*wapp));
    strncpy(wapp->name, name, WAPP_MAX_NAME_LEN - 1);

    /* The launch attempt owns the reservation: copy its config onto the wapp
     * and clear the slot now, so a failed load frees the pending pool and a
     * later start never silently reuses stale config. */
    if (pend != NULL) {
        wapp->cfg = pend->cfg;
        memset(pend, 0, sizeof(*pend));
    }

    /* Instance identity (wapp->name) is the path-supplied `name`; the loader
     * resolves the image's registry entry and stamps image identity onto the
     * wapp, never touching wapp->name. */
    const char *img = image;
    if (!haveImage)
        img = wapp->cfg.image[0] ? wapp->cfg.image : name;

    /* An image reference is "<name>[:<tag>]": a bare name takes the first
     * match, a tag pins the version. ':' is the wapp-visible separator; the
     * loader maps the pair onto REGISTRY_VERSION_SEPARATOR on disk. */
    memset(&e, 0, sizeof(e));
    const char *colon = strchr(img, ':');
    if (colon != NULL) {
        size_t nlen = (size_t)(colon - img);
        if (nlen >= WAPP_MAX_NAME_LEN)
            nlen = WAPP_MAX_NAME_LEN - 1;
        memcpy(e.name, img, nlen);
        e.name[nlen] = '\0';
        strncpy(e.version, colon + 1, WAPP_MAX_VERSION_LEN - 1);
        e.version[WAPP_MAX_VERSION_LEN - 1] = '\0';
    } else {
        strncpy(e.name, img, WAPP_MAX_NAME_LEN - 1);
        e.name[WAPP_MAX_NAME_LEN - 1] = '\0';
        e.version[0] = '\0';
    }

    ret = PlatformRegistryWappLoad(&e, wapp);
    if (ret < 0)
        goto FREE; /* nothing mapped yet */

    ret = PlatformWappStart(wapp);
    if (ret < 0)
        goto UNLOAD;

    /* Ownership of `wapp` and its mapped image transfers to the platform
     * thread slot, which releases it when reused. Freeing here would be a
     * use-after-free against the just-spawned thread. */
    return ret;

UNLOAD:
    PlatformWappUnload(wapp);
FREE:
    WantedFree(wapp);
    return ret;
}

/* ── Driver ops ─────────────────────────────────────────────────────────── */

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *c = d->ctx;
    WantedFree(c->pending); /* lazily reallocated by the next supervisor */
    memset(c, 0, sizeof(ctx));
    return 0;
}

/* True if the engine knows this wapp name — it is live (a platform slot) or has
 * a `create` reservation. The per-wapp namespace exists only for a known wapp.
 */
static bool wappKnown(struct vfs_driver_ctx_t *d, const char *name) {
    wapp_state_t st;
    return lookupState(name, &st) || pending_find(d, name) != NULL;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;
    wapp_node_t node;
    char name[WAPP_MAX_NAME_LEN];

    int ret = resolvePath(path, &node, name);
    if (ret < 0)
        return ret;

    /* A wapp's directory and its nodes exist only once `create` has brought
     * the wapp into being. An unknown name returns ENOENT, so a name cannot be
     * probed by guessing its path. */
    if (node != NODE_ROOT && !wappKnown(d, name))
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

    bool isDir = d->fds[fd].node == NODE_ROOT || d->fds[fd].node == NODE_WAPP;

    stat->dev = WantedWappsDriver.bytesId;
    stat->ino = 0;
    stat->filetype =
        isDir ? VFS_FILETYPE_DIRECTORY : VFS_FILETYPE_CHARACTER_DEVICE;
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
static size_t renderRead(wapp_node_t node, const char *name, char *out,
                         size_t cap) {
    wapp_state_t st;
    bool live = lookupState(name, &st);

    switch (node) {
    case NODE_STATE: {
        /* For a wapp the platform does not run yet, the reservation separates
         * a bare `create` (no config → created) from one whose config has been
         * written (→ not_started). An unknown name is that same default. */
        status_t s;
        if (live) {
            s = st.status;
        } else {
            const wapps_pending_t *p = pending_find(&ctx, name);
            s = (p != NULL && !p->configured) ? CREATED : NOT_STARTED;
        }
        return (size_t)snprintf(out, cap, "%s", StatusToString(s));
    }
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

    char line[WAPPS_LINE_MAX];
    size_t n = renderRead(node, d->fds[fd].name, line, sizeof(line));
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
            if (*img != '\0' && strlen(img) >= WAPP_MAX_IMAGE_REF_LEN)
                return -EINVAL;
            ret = startWapp(d, name, img[0] ? img : NULL);
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
static bool packDirent(void *buf, size_t bufLen, size_t *used, uint64_t ino,
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
        wapp_state_t states[CONFIG_WANTED_MAX_WAPPS];
        int n = PlatformWappGetState(states, CONFIG_WANTED_MAX_WAPPS);
        if (n < 0)
            return n;

        /* Enumerate the live wapps plus any `create`d-but-not-running ones
         * (reserved pending slots), de-duplicated by name, into one list so the
         * cookie can page over a stable index space. */
        char names[2 * CONFIG_WANTED_MAX_WAPPS][WAPP_MAX_NAME_LEN];
        int total = 0;
        for (int k = 0; k < n; k++) {
            strncpy(names[total], states[k].name, WAPP_MAX_NAME_LEN - 1);
            names[total][WAPP_MAX_NAME_LEN - 1] = '\0';
            total++;
        }
        for (int k = 0; d->pending != NULL && k < CONFIG_WANTED_MAX_WAPPS;
             k++) {
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
            if (!packDirent(buf, bufLen, &used, i, i + 1,
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
            if (!packDirent(buf, bufLen, &used, i, i + 1,
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

/* WantedCtlDriver — the root /dev/wanted/ctl node, a write-only command channel
 * reachable only by a wapp granted the /dev/wanted driver. It does not launch
 * wapps; that is wapps/<name>/ctl. Verbs: see the control-plane doc. */

#define CTL_ID {'W', 'c', 't', 'l'}

static int _ctl_Destroy(struct vfs_driver_t *d) {
    (void)d;
    return 0;
}

static int _ctl_Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
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
     * whole token so the bounded line buffer is never read past its NUL. */
    if (strncmp(line, "poweroff", sizeof("poweroff")) == 0) {
        PlatformRequestShutdown();
        return (int)nbyte;
    }
    if (strncmp(line, "reboot", sizeof("reboot")) == 0) {
        PlatformRequestReboot();
        return (int)nbyte;
    }

    /* Arm a supervisor image reload — applied at the next respawn. */
    if (strncmp(line, "reload-supervisor", sizeof("reload-supervisor")) == 0) {
        WantedSupervisorReload();
        return (int)nbyte;
    }

    /* Pin the compiled-in supervisor image and arm a reload, so a supervisor
     * that cannot support this engine falls back deterministically instead of
     * waiting on the respawn loop's ceiling. -EALREADY when it already runs. */
    if (strncmp(line, "rollback-supervisor", sizeof("rollback-supervisor")) ==
        0) {
        if (WantedSupervisorRollback() != 0)
            return -EALREADY;
        return (int)nbyte;
    }

    /* "create <name>": register the per-wapp namespace ahead of a config write
     * and start, without launching. Reserves a zeroed slot so wapps/<name>/
     * enumerates and a later config write targets it. */
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
     * nodes return -ENOENT again. Frees a reservation and/or a terminal
     * platform slot; a running wapp is -EBUSY, an unknown name -ENOENT. */
    static const char DELETE_VERB[] = "delete ";
    size_t dlen = sizeof(DELETE_VERB) - 1;
    if (strncmp(line, DELETE_VERB, dlen) == 0) {
        const char *dname = line + dlen;
        while (*dname == ' ' || *dname == '\t')
            dname++;
        if (*dname == '\0' || strlen(dname) >= WAPP_MAX_NAME_LEN)
            return -EINVAL;

        wapp_state_t st;
        bool live = lookupState(dname, &st);
        wapps_pending_t *p = pending_find(&ctx, dname);
        if (!live && p == NULL)
            return -ENOENT;

        /* Release the platform slot first: a running wapp returns -EBUSY and
         * leaves the reservation intact, so a retry after a stop does not lose
         * buffered config to a half-applied delete. */
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
