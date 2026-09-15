/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX WiFi driver: /dev/wifi/{status,scan,ctl}, so the wapp stays pure
 * WASI. The radio is reached through the WAPI library on wlan0; see the VFS
 * reference for the wire text. The driver owns station reconnection; it never
 * hosts an access point — bcm43xxx has no AP write path, and mapping ap_start
 * onto WAPI_MODE_MASTER would silently select ad-hoc instead, so ap_start and
 * ap_stop both answer -ENODEV unconditionally. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

/* The real radio path needs the WAPI library and netlib, which exist only on a
 * wireless-capable config. Gate it on CONFIG_WIRELESS_WAPI so the sim links the
 * in-memory stub rather than absent symbols. */
#if defined(__NuttX__) && defined(CONFIG_WIRELESS_WAPI)
#define WIFI_HW 1
#else
#define WIFI_HW 0
#endif

#if WIFI_HW
#include <pthread.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netutils/netlib.h>
#include <nuttx/wireless/wireless.h>
#include <wireless/wapi.h>
#endif

#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'W', 'i', 'f', 'i'};

#define WIFI_IFNAME "wlan0"
#define WIFI_MAX_FDS 4           /* concurrent opens across status/scan/ctl */
#define WIFI_SSID_MAX 33         /* 32 + NUL */
#define WIFI_PASS_MAX 64         /* 63 + NUL */
#define WIFI_SCAN_TRIES 20       /* poll the scan result this many times... */
#define WIFI_SCAN_WAIT_US 500000 /* ...waiting this long between polls */
#define WIFI_CMD_MAX 128         /* longest accepted command line */

#define WIFI_NODE_ROOT 0
#define WIFI_NODE_STATUS 1
#define WIFI_NODE_SCAN 2
#define WIFI_NODE_CTL 3

/* No WAPI async-disconnect event exists, so a background thread polls the
 * associated BSSID at this cadence to notice an unsolicited drop. */
#define WIFI_MONITOR_POLL_MS 2000
#define WIFI_RECONNECT_MIN_DELAY_MS 1000
#define WIFI_RECONNECT_MAX_DELAY_MS 30000

struct wifi_fd_t {
    bool used;
    uint8_t node;
    /* A status read latches per descriptor, re-arming whenever the radio's
     * state changes — g_wifiGen bumps on every such change. */
    uint32_t seenGen;
    char *scan; /* heap scan-result text, drained by reads */
    size_t scan_len;
    size_t scan_off;
};

struct vfs_driver_ctx_t {
    struct wifi_fd_t fds[WIFI_MAX_FDS];
};

/* Module-level, not per-descriptor: a fire-and-forget wapp raises a link and
 * exits, and the link — and the intent to hold it — must survive it. Guarded
 * by g_wifiMutex, since the monitor thread below reads and updates it
 * concurrently with VFS calls on a wapp's own thread. */
static bool g_wifiStarted;
static bool g_wifiConnected;
static bool g_hasIntent;
static char g_wifiIfname[16] = WIFI_IFNAME;
static char g_staSsid[WIFI_SSID_MAX];
static char g_staPass[WIFI_PASS_MAX];
static char g_wifiIp[16] = "0.0.0.0";
static uint32_t g_wifiGen = 1; /* never 0, so a fresh fd's seenGen=0 misses */
static uint32_t g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
static platform_mutex_t *g_wifiMutex;
#if WIFI_HW
static pthread_t g_monitorThread;
#endif

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

#if WIFI_HW

/* Run a blocking scan and return its results as one malloc'd text block of
 * "<ssid> <bssid> <rssi>\n" lines (caller owns it). NULL on failure. */
static char *scanCollect(const char *ifname) {
    int sock = wapi_make_socket();
    if (sock < 0)
        return NULL;

    char *out = NULL;
    if (wapi_scan_init(sock, ifname, NULL) < 0)
        goto done;

    int ready = -1;
    for (int i = 0; i < WIFI_SCAN_TRIES; i++) {
        ready = wapi_scan_stat(sock, ifname);
        if (ready <= 0) /* 0 = ready, <0 = failure */
            break;
        usleep(WIFI_SCAN_WAIT_US);
    }
    if (ready != 0)
        goto done;

    struct wapi_list_s list;
    memset(&list, 0, sizeof(list));
    if (wapi_scan_coll(sock, ifname, &list) < 0)
        goto done;

    /* Two passes: size the buffer, then fill it. */
    char line[WIFI_SSID_MAX + 64];
    size_t total = 1; /* trailing NUL */
    for (struct wapi_scan_info_s *ap = list.head.scan; ap; ap = ap->next) {
        int n = snprintf(
            line, sizeof(line), "%s %02x:%02x:%02x:%02x:%02x:%02x %d\n",
            ap->has_essid ? ap->essid : "", ap->ap.ether_addr_octet[0],
            ap->ap.ether_addr_octet[1], ap->ap.ether_addr_octet[2],
            ap->ap.ether_addr_octet[3], ap->ap.ether_addr_octet[4],
            ap->ap.ether_addr_octet[5], ap->has_rssi ? ap->rssi : 0);
        if (n > 0)
            total += (size_t)n;
    }

    out = (char *)WantedMalloc(total);
    if (out != NULL) {
        size_t off = 0;
        for (struct wapi_scan_info_s *ap = list.head.scan; ap; ap = ap->next) {
            int n = snprintf(
                out + off, total - off, "%s %02x:%02x:%02x:%02x:%02x:%02x %d\n",
                ap->has_essid ? ap->essid : "", ap->ap.ether_addr_octet[0],
                ap->ap.ether_addr_octet[1], ap->ap.ether_addr_octet[2],
                ap->ap.ether_addr_octet[3], ap->ap.ether_addr_octet[4],
                ap->ap.ether_addr_octet[5], ap->has_rssi ? ap->rssi : 0);
            if (n > 0)
                off += (size_t)n;
        }
        out[off] = '\0';
    }

    wapi_scan_coll_free(&list);

done:
    close(sock);
    return out;
}

/* True while wapi_get_ap() reports a non-null BSSID: a null address is what
 * the library returns for "off" — not associated. This is the only liveness
 * signal available; WAPI raises no disconnect event to poll instead. */
static bool linkIsAssociated(const char *ifname) {
    int sock = wapi_make_socket();
    if (sock < 0)
        return false;
    struct ether_addr ap;
    memset(&ap, 0, sizeof(ap));
    int rc = wapi_get_ap(sock, ifname, &ap);
    close(sock);
    if (rc < 0)
        return false;
    for (size_t i = 0; i < sizeof(ap.ether_addr_octet); i++) {
        if (ap.ether_addr_octet[i] != 0)
            return true;
    }
    return false;
}

/* Associate to an open/WPA2-PSK network and acquire an address over DHCP.
 * Touches no shared state — the caller publishes the outcome under the lock —
 * so a blocking associate never holds g_wifiMutex across it. */
static int wifiAssociate(const char *ifname, const char *ssid, const char *pass,
                         char ipOut[16]) {
    struct wpa_wconfig_s conf;
    memset(&conf, 0, sizeof(conf));
    conf.ifname = ifname;
    conf.sta_mode = WAPI_MODE_MANAGED;
    conf.ssid = ssid;
    conf.ssidlen = strlen(ssid);
    conf.passphrase = pass;
    conf.phraselen = strlen(pass);
    conf.bssid = NULL;

    if (pass[0] == '\0') {
        conf.auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
        conf.cipher_mode = IW_AUTH_CIPHER_NONE;
        conf.alg = WPA_ALG_NONE;
    } else {
        conf.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
        conf.cipher_mode = IW_AUTH_CIPHER_CCMP;
        conf.alg = WPA_ALG_CCMP;
    }

    int ret = wpa_driver_wext_associate(&conf);
    if (ret < 0)
        return ret;

    /* Best effort: a failed lease still leaves the link associated. */
    netlib_obtain_ipv4addr(ifname);

    struct in_addr ip;
    memset(&ip, 0, sizeof(ip));
    netlib_get_ipv4addr(ifname, &ip);
    strncpy(ipOut, inet_ntoa(ip), 15);
    ipOut[15] = '\0';
    return 0;
}

static void wifiDisconnectNow(const char *ifname) {
    int sock = wapi_make_socket();
    if (sock < 0)
        return;
    wpa_driver_wext_disconnect(sock, ifname);
    close(sock);
}

/* Retries a stored intent on a fixed poll cadence, with a capped backoff
 * between failed attempts, for as long as the intent stands — MDR-0047's
 * "disconnected means no intent stored, never gave up", applied without an
 * async disconnect event to trigger off. Runs for the life of the process:
 * the wapp that stored the intent may already be gone. */
static void *wifiMonitorThread(void *arg) {
    (void)arg;
    for (;;) {
        usleep(WIFI_MONITOR_POLL_MS * 1000);

        PlatformMutexLock(g_wifiMutex);
        bool hasIntent = g_hasIntent;
        bool wasConnected = g_wifiConnected;
        char ifname[sizeof(g_wifiIfname)];
        char ssid[WIFI_SSID_MAX];
        char pass[WIFI_PASS_MAX];
        strncpy(ifname, g_wifiIfname, sizeof(ifname));
        strncpy(ssid, g_staSsid, sizeof(ssid));
        strncpy(pass, g_staPass, sizeof(pass));
        PlatformMutexUnlock(g_wifiMutex);

        if (!hasIntent)
            continue;

        if (wasConnected) {
            if (linkIsAssociated(ifname))
                continue; /* still up */
            PlatformMutexLock(g_wifiMutex);
            if (g_hasIntent && strcmp(g_staSsid, ssid) == 0) {
                g_wifiConnected = false;
                strncpy(g_wifiIp, "0.0.0.0", sizeof(g_wifiIp) - 1);
                g_wifiIp[sizeof(g_wifiIp) - 1] = '\0';
                g_wifiGen++;
            }
            PlatformMutexUnlock(g_wifiMutex);
            continue; /* try the reconnect itself on the next tick */
        }

        char ip[16];
        int rc = wifiAssociate(ifname, ssid, pass, ip);

        PlatformMutexLock(g_wifiMutex);
        /* The stored intent may have changed while this ran unlocked; only
         * publish the outcome if it is still the intent this attempt was
         * for. */
        if (g_hasIntent && strcmp(g_staSsid, ssid) == 0) {
            if (rc == 0) {
                g_wifiConnected = true;
                strncpy(g_wifiIp, ip, sizeof(g_wifiIp) - 1);
                g_wifiIp[sizeof(g_wifiIp) - 1] = '\0';
                g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
            } else {
                uint32_t next = g_reconnectDelayMs * 2;
                g_reconnectDelayMs = (next > WIFI_RECONNECT_MAX_DELAY_MS)
                                         ? WIFI_RECONNECT_MAX_DELAY_MS
                                         : next;
            }
            g_wifiGen++;
        }
        uint32_t delay = g_reconnectDelayMs;
        PlatformMutexUnlock(g_wifiMutex);

        if (rc != 0)
            usleep(delay * 1000);
    }
    return NULL;
}

#endif /* WIFI_HW */

/* One-time bring-up: the interface, the state mutex and — on real hardware —
 * the reconnect monitor thread. Idempotent, so every wapp granted the driver
 * finds the radio ready. */
static void wifiEnsureStarted(void) {
    if (g_wifiStarted)
        return;

    g_wifiMutex = PlatformMutexNew();

#if WIFI_HW
    netlib_ifup(g_wifiIfname);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
#ifdef PTHREAD_STACK_MIN
    pthread_attr_setstacksize(&attr, PTHREAD_STACK_MIN);
#endif
    if (pthread_create(&g_monitorThread, &attr, wifiMonitorThread, NULL) == 0)
        pthread_detach(g_monitorThread);
    pthread_attr_destroy(&attr);
#endif

    g_wifiStarted = true;
}

vfs_driver_t *VfsWifiInit(const wapp_t *wapp, const char *options) {
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

    /* The interface name is fixed for the process once the radio is up — one
     * physical radio, so a later grant's options cannot retarget it. */
    if (!g_wifiStarted && options != NULL && options[0] != '\0') {
        strncpy(g_wifiIfname, options, sizeof(g_wifiIfname) - 1);
        g_wifiIfname[sizeof(g_wifiIfname) - 1] = '\0';
    }
    wifiEnsureStarted();

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

    return driver;
}

static int _Destroy(struct vfs_driver_t *d) {
    struct vfs_driver_ctx_t *ctx = d->ctx;
    for (int i = 0; i < WIFI_MAX_FDS; i++)
        WantedFree(ctx->fds[i].scan);
    WantedFree(ctx);
    WantedFree(d);
    return 0;
}

static int resolve(const char *path, uint8_t *node) {
    const char *p = path;
    while (*p == '/')
        p++;
    if (*p == '\0')
        *node = WIFI_NODE_ROOT;
    else if (strcmp(p, "status") == 0)
        *node = WIFI_NODE_STATUS;
    else if (strcmp(p, "scan") == 0)
        *node = WIFI_NODE_SCAN;
    else if (strcmp(p, "ctl") == 0)
        *node = WIFI_NODE_CTL;
    else
        return -ENOENT;
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)flags;
    uint8_t node;
    int rc = resolve(path != NULL ? path : "", &node);
    if (rc < 0)
        return rc;

    for (int i = 0; i < WIFI_MAX_FDS; i++) {
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
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    WantedFree(d->fds[fd].scan);
    memset(&d->fds[fd], 0, sizeof(d->fds[fd]));
    return 0;
}

static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *s) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    s->filetype = (d->fds[fd].node == WIFI_NODE_ROOT)
                      ? VFS_FILETYPE_DIRECTORY
                      : VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

/* One line: disconnected, connecting (initial or retry alike — NuttX never
 * hosts an AP, so that line never appears here), or connected <ssid> <ip>. */
static size_t renderStatus(char *line, size_t lineLen) {
    int n;
    PlatformMutexLock(g_wifiMutex);
    if (g_wifiConnected)
        n = snprintf(line, lineLen, "connected %s %s\n", g_staSsid, g_wifiIp);
    else if (g_hasIntent)
        n = snprintf(line, lineLen, "connecting\n");
    else
        n = snprintf(line, lineLen, "disconnected\n");
    PlatformMutexUnlock(g_wifiMutex);
    return (n < 0) ? 0 : (size_t)n;
}

static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct wifi_fd_t *f = &d->fds[fd];

    if (f->node == WIFI_NODE_ROOT)
        return -EISDIR;
    if (f->node == WIFI_NODE_CTL)
        return -EPERM; /* write-only */

    if (f->node == WIFI_NODE_SCAN) {
        if (f->scan == NULL)
            return 0; /* no scan run yet on this descriptor */
        size_t left = f->scan_len - f->scan_off;
        if (left == 0) {
            WantedFree(f->scan);
            f->scan = NULL;
            f->scan_off = f->scan_len = 0;
            return 0; /* EOF for the scan stream */
        }
        size_t n = (nbyte < left) ? nbyte : left;
        memcpy(buf, f->scan + f->scan_off, n);
        f->scan_off += n;
        return (int)n;
    }

    /* WIFI_NODE_STATUS */
    uint32_t gen;
    PlatformMutexLock(g_wifiMutex);
    gen = g_wifiGen;
    PlatformMutexUnlock(g_wifiMutex);
    if (f->seenGen == gen)
        return 0;

    char line[WIFI_SSID_MAX + 32];
    size_t len = renderStatus(line, sizeof(line));
    size_t n = (nbyte < len) ? nbyte : len;
    memcpy(buf, line, n);
    f->seenGen = gen;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct wifi_fd_t *f = &d->fds[fd];
    if (f->node == WIFI_NODE_ROOT)
        return -EISDIR;
    if (f->node == WIFI_NODE_STATUS)
        return -EPERM; /* read-only */
    if (nbyte == 0)
        return 0;

    if (f->node == WIFI_NODE_SCAN) {
        /* A scan result reads like a command reply: writing to the node that
         * carries it would collapse state and action onto the same file, the
         * shape MDR-0047 replaces. Scan is triggered from ctl. */
        return -EPERM;
    }

    char cmd[WIFI_CMD_MAX];
    size_t len = (nbyte < sizeof(cmd) - 1) ? nbyte : sizeof(cmd) - 1;
    memcpy(cmd, buf, len);
    cmd[len] = '\0';
    /* Trim a trailing newline so callers may or may not send one. */
    if (len > 0 && cmd[len - 1] == '\n')
        cmd[len - 1] = '\0';

    if (strcmp(cmd, "scan") == 0) {
#if WIFI_HW
        WantedFree(f->scan);
        f->scan = scanCollect(g_wifiIfname);
        if (f->scan == NULL)
            return -EIO;
        f->scan_len = strlen(f->scan);
        f->scan_off = 0;
#else
        const char stub[] = "stub-ap 00:00:00:00:00:00 -42\n";
        WantedFree(f->scan);
        f->scan = (char *)WantedMalloc(sizeof(stub));
        if (f->scan == NULL)
            return -ENOMEM;
        memcpy(f->scan, stub, sizeof(stub));
        f->scan_len = sizeof(stub) - 1;
        f->scan_off = 0;
#endif
        return (int)nbyte;
    }

    if (strncmp(cmd, "connect", 7) == 0) {
        /* "connect <ssid> [pass]" — split on the two spaces. */
        char *ssid = cmd + 7;
        while (*ssid == ' ')
            ssid++;
        if (*ssid == '\0')
            return -EINVAL;
        char *pass = ssid;
        while (*pass != '\0' && *pass != ' ')
            pass++;
        if (*pass == ' ')
            *pass++ = '\0';

        char ifname[sizeof(g_wifiIfname)];
        PlatformMutexLock(g_wifiMutex);
        strncpy(g_staSsid, ssid, sizeof(g_staSsid) - 1);
        g_staSsid[sizeof(g_staSsid) - 1] = '\0';
        strncpy(g_staPass, pass, sizeof(g_staPass) - 1);
        g_staPass[sizeof(g_staPass) - 1] = '\0';
        g_hasIntent = true;
        g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
        strncpy(ifname, g_wifiIfname, sizeof(ifname));
        g_wifiGen++;
        PlatformMutexUnlock(g_wifiMutex);

#if WIFI_HW
        char ip[16];
        int rc = wifiAssociate(ifname, ssid, pass, ip);
        PlatformMutexLock(g_wifiMutex);
        if (g_hasIntent && strcmp(g_staSsid, ssid) == 0) {
            if (rc == 0) {
                g_wifiConnected = true;
                strncpy(g_wifiIp, ip, sizeof(g_wifiIp) - 1);
                g_wifiIp[sizeof(g_wifiIp) - 1] = '\0';
            }
            g_wifiGen++;
        }
        PlatformMutexUnlock(g_wifiMutex);
        return (rc < 0) ? -EIO : (int)nbyte;
#else
        PlatformMutexLock(g_wifiMutex);
        g_wifiConnected = true;
        g_wifiGen++;
        PlatformMutexUnlock(g_wifiMutex);
        return (int)nbyte;
#endif
    }

    if (strcmp(cmd, "disconnect") == 0) {
        char ifname[sizeof(g_wifiIfname)];
        PlatformMutexLock(g_wifiMutex);
        g_hasIntent = false;
        g_wifiConnected = false;
        strncpy(g_wifiIp, "0.0.0.0", sizeof(g_wifiIp) - 1);
        g_wifiIp[sizeof(g_wifiIp) - 1] = '\0';
        strncpy(ifname, g_wifiIfname, sizeof(ifname));
        g_wifiGen++;
        PlatformMutexUnlock(g_wifiMutex);
#if WIFI_HW
        wifiDisconnectNow(ifname);
#else
        (void)ifname;
#endif
        return (int)nbyte;
    }

    /* Neither verb is implemented: bcm43xxx has no AP write path, and
     * mapping ap_start onto WAPI_MODE_MASTER would silently select ad-hoc
     * rather than host an AP. -ENODEV is the honest answer for both. */
    if (strcmp(cmd, "ap_start") == 0 || strcmp(cmd, "ap_stop") == 0)
        return -ENODEV;

    return -EINVAL;
}

static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (d->fds[fd].node != WIFI_NODE_ROOT)
        return -ENOTDIR;

    static const vfs_dir_entry_t entries[] = {
        {"status", VFS_FILETYPE_CHARACTER_DEVICE},
        {"scan", VFS_FILETYPE_CHARACTER_DEVICE},
        {"ctl", VFS_FILETYPE_CHARACTER_DEVICE},
    };
    return VfsFlatDirReadDir(entries, 3, buf, bufLen, cookie, bufUsed);
}
