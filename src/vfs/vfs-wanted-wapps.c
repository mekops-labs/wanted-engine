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

/* ── WantedWappsDriver ───────────────────────────────────────────────────────
 *
 * The per-wapp control namespace mounted at /dev/wanted/wapps. The path
 * carries wapp identity, reads are composable plain text, and only the start
 * config is JSON.
 *
 *   wapps/                 ReadDir → one entry per known (running) wapp
 *     <name>/              synthetic dir; ReadDir → the control files below
 *       ctl       (w)      line verb: "start" | "stop"  (identity = path)
 *       state     (r)      plain-text token: not_started|starting|running|...
 *       version   (r)      plain-text version, e.g. "1.0.0-0"
 *       id        (r)      plain-text engine wapp id
 *       config    (w)      JSON start config: { console, drivers[], preopens }
 *
 * Every open allocates its own slot in the fd table below; read EOF is latched
 * per-fd, so concurrent readers (the supervisor reopens on every tick) keep
 * independent cursors.
 * ───────────────────────────────────────────────────────────────────────── */

#define ID                                                                     \
    { 'W', 'a', 'p', 'p' }

/* Longest control verb / read token we emit; bounds the write line buffer so
 * a write() never drives an unbounded (VLA) stack allocation. */
#define WAPPS_LINE_MAX 32

typedef enum {
    NODE_NONE = 0,
    NODE_ROOT,    /* wapps/            */
    NODE_WAPP,    /* wapps/<name>/     */
    NODE_CTL,     /* wapps/<name>/ctl     */
    NODE_STATE,   /* wapps/<name>/state   */
    NODE_VERSION, /* wapps/<name>/version */
    NODE_ID,      /* wapps/<name>/id      */
    NODE_CONFIG,  /* wapps/<name>/config  */
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
 * ceiling the runtime imposes on live wapps. */
typedef struct {
    bool valid;
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
    {"ctl", NODE_CTL},     {"state", NODE_STATE}, {"version", NODE_VERSION},
    {"id", NODE_ID},       {"config", NODE_CONFIG},
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
 * Identity comes from `name` (supplied by the path), not a payload field. */
static int StartWapp(struct vfs_driver_ctx_t *d, const char *name) {
    int ret;
    reg_entry_t e;
    wapp_t *wapp = WantedMalloc(sizeof(wapp_t));
    if (NULL == wapp)
        return -ENOMEM;

    memset(wapp, 0, sizeof(*wapp));
    strncpy(wapp->name, name, WAPP_MAX_NAME_LEN - 1);

    memset(&e, 0, sizeof(e));
    strncpy(e.name, name, WAPP_MAX_NAME_LEN - 1);
    e.version[0] = '\0';

    ret = PlatformRegistryWappLoad(&e, wapp);
    if (ret < 0)
        goto FREE; /* nothing mapped yet */

    ret = WantedWappParseManifest(wapp);
    if (ret < 0)
        goto UNLOAD;

    wapps_pending_t *p = pending_find(d, name);
    if (p != NULL)
        wapp->cfg = p->cfg;

    ret = PlatformWappStart(wapp);
    if (ret < 0)
        goto UNLOAD;

    /* Config consumed by the launch — clear it so a later start does not
     * silently reuse stale grants. */
    if (p != NULL)
        memset(p, 0, sizeof(*p));

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

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;
    wapp_node_t node;
    char name[WAPP_MAX_NAME_LEN];

    int ret = ResolvePath(path, &node, name);
    if (ret < 0)
        return ret;

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
    case NODE_STATE:
        return (size_t)snprintf(out, cap, "%s",
                                live ? statusToString(st.status)
                                     : statusToString(NOT_STARTED));
    case NODE_VERSION: {
        const uint8_t *v = live ? st.version.v : (const uint8_t[]){0, 0, 0, 0};
        return (size_t)snprintf(out, cap, "%X.%X.%X-%X", v[0], v[1], v[2],
                                v[3]);
    }
    case NODE_ID:
        return (size_t)snprintf(out, cap, "%u", live ? st.id : 0);
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
        if (strcmp(line, "start") == 0) {
            ret = StartWapp(d, name);
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
        uint64_t i = *cookie;
        for (; i < (uint64_t)n; i++) {
            size_t nameLen = strnlen(states[i].name, WAPP_MAX_NAME_LEN);
            if (!PackDirent(buf, bufLen, &used, i, i + 1,
                            VFS_FILETYPE_DIRECTORY, states[i].name, nameLen))
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
 * The root /dev/wanted/ctl node. Write-only create-and-launch shorthand:
 *
 *   echo "start <name>" > /dev/wanted/ctl
 *
 * Identity travels in the verb argument, not a JSON payload. Any config
 * previously buffered at wapps/<name>/config is applied to the launch.
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

    /* expect: "start <name>" */
    static const char VERB[] = "start ";
    size_t vlen = sizeof(VERB) - 1;
    if (strncmp(line, VERB, vlen) != 0)
        return -EINVAL;

    const char *name = line + vlen;
    while (*name == ' ' || *name == '\t')
        name++;
    if (*name == '\0' || strlen(name) >= WAPP_MAX_NAME_LEN)
        return -EINVAL;

    int ret = StartWapp(&ctx, name);
    return ret < 0 ? ret : (int)nbyte;
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
