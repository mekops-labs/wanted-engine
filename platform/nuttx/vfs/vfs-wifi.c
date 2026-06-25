/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX WiFi station driver, exposed to a wapp as a text command/status node.
 *
 * A wapp granted the `wifi` driver gets /dev/wifi in its namespace; the engine
 * drives the radio on the wapp's behalf and exposes only a text contract, so
 * the wapp stays pure WASI (WASI has no ioctl):
 *   write "scan"                  -> start a scan; following reads stream one
 *                                    "<ssid> <bssid> <rssi>\n" line per AP,
 *                                    then EOF
 *   write "connect <ssid> <pass>" -> associate (WPA2-PSK/CCMP) and run DHCP
 *   write "disconnect"            -> drop the association
 *   read (no pending scan)        -> one status line: "disconnected\n" or
 *                                    "connected <ssid> <ip>\n" (<ip> is the
 *                                    DHCP lease, or 0.0.0.0 if none)
 *
 * The radio is reached through the NuttX WAPI library on the wlan0 interface.
 * On the host-only scaffolding build the WAPI/NuttX wireless headers are
 * absent, so the node holds state in memory; the real path compiles only for
 * __NuttX__. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __NuttX__
#include <unistd.h>

#include <arpa/inet.h>
#include <netutils/netlib.h>
#include <nuttx/wireless/wireless.h>
#include <wireless/wapi.h>
#endif

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'W', 'i', 'f', 'i'};

#define WIFI_IFNAME "wlan0"
#define WIFI_MAX_FDS 2           /* concurrent opens of the node */
#define WIFI_SSID_MAX 33         /* 32 + NUL */
#define WIFI_SCAN_TRIES 20       /* poll the scan result this many times... */
#define WIFI_SCAN_WAIT_US 500000 /* ...waiting this long between polls */
#define WIFI_CMD_MAX 128         /* longest accepted command line */

enum wifi_state_t {
    WIFI_DISCONNECTED = 0,
    WIFI_CONNECTED,
};

struct wifi_fd_t {
    bool used;
    bool status_done; /* per-fd EOF latch for a status read */
    char *scan;       /* heap scan-result text, drained by reads */
    size_t scan_len;
    size_t scan_off;
};

struct vfs_driver_ctx_t {
    char ifname[16];
    enum wifi_state_t state;
    char ssid[WIFI_SSID_MAX];
    char ip[16]; /* leased IPv4, dotted-quad; "0.0.0.0" when no DHCP lease */
    struct wifi_fd_t fds[WIFI_MAX_FDS];
};

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

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

    const char *ifname =
        (options != NULL && options[0] != '\0') ? options : WIFI_IFNAME;
    strncpy(ctx->ifname, ifname, sizeof(ctx->ifname) - 1);
    ctx->state = WIFI_DISCONNECTED;

    /* Bring the interface up as part of driver setup: scan and connect both
     * need the WiFi station started (ifup -> esp_wifi_start), else
     * esp_wifi_scan_start / association return ESP_ERR_WIFI_NOT_STARTED.
     * Idempotent — a no-op once wlan0 is up. Done here (not per-op) so every
     * wifi operation a granted wapp makes finds the radio started. */
    netlib_ifup(ctx->ifname);

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
    for (int i = 0; i < WIFI_MAX_FDS; i++)
        WantedFree(ctx->fds[i].scan);
    WantedFree(ctx);
    WantedFree(d);
    return 0;
}

static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags) {
    (void)path;
    (void)flags;
    for (int i = 0; i < WIFI_MAX_FDS; i++) {
        if (!d->fds[i].used) {
            memset(&d->fds[i], 0, sizeof(d->fds[i]));
            d->fds[i].used = true;
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
    (void)d;
    (void)fd;
    memset(s, 0, sizeof(*s));
    s->dev = *(const uint32_t *)(id);
    s->filetype = VFS_FILETYPE_CHARACTER_DEVICE;
    return 0;
}

#ifdef __NuttX__

/* Run a blocking scan and return its results as one malloc'd text block of
 * "<ssid> <bssid> <rssi>\n" lines (caller owns it). NULL on failure. */
static char *scan_collect(const char *ifname) {
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

/* Associate to an open/WPA2-PSK network and acquire an address over DHCP. */
static int wifi_connect(struct vfs_driver_ctx_t *d, const char *ssid,
                        const char *pass) {
    struct wpa_wconfig_s conf;
    memset(&conf, 0, sizeof(conf));
    conf.ifname = d->ifname;
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
    netlib_obtain_ipv4addr(d->ifname);

    /* Read the address back and surface it in the state node: 0.0.0.0 means no
     * lease (link associated only), anything else is the leased IPv4. */
    struct in_addr ip;
    memset(&ip, 0, sizeof(ip));
    netlib_get_ipv4addr(d->ifname, &ip);
    strncpy(d->ip, inet_ntoa(ip), sizeof(d->ip) - 1);
    d->ip[sizeof(d->ip) - 1] = '\0';
    DEBUG_TRACE("DHCP on %s -> %s", d->ifname, d->ip);

    d->state = WIFI_CONNECTED;
    strncpy(d->ssid, ssid, sizeof(d->ssid) - 1);
    d->ssid[sizeof(d->ssid) - 1] = '\0';
    return 0;
}

static int wifi_disconnect(struct vfs_driver_ctx_t *d) {
    int sock = wapi_make_socket();
    if (sock < 0)
        return -errno;
    wpa_driver_wext_disconnect(sock, d->ifname);
    close(sock);
    d->state = WIFI_DISCONNECTED;
    d->ssid[0] = '\0';
    d->ip[0] = '\0';
    return 0;
}

#endif /* __NuttX__ */

/* read: drain a pending scan result, else return one status line then EOF. */
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    struct wifi_fd_t *f = &d->fds[fd];

    if (f->scan != NULL) {
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

    if (f->status_done)
        return 0;

    char line[WIFI_SSID_MAX + 32];
    int n;
    if (d->state == WIFI_CONNECTED)
        n = snprintf(line, sizeof(line), "connected %s %s\n", d->ssid,
                     d->ip[0] ? d->ip : "0.0.0.0");
    else
        n = snprintf(line, sizeof(line), "disconnected\n");
    if (n < 0)
        return -EIO;

    size_t len = (size_t)n;
    size_t out = (nbyte < len) ? nbyte : len;
    memcpy(buf, line, out);
    f->status_done = true;
    return (int)out;
}

/* write: a text command — "scan", "connect <ssid> <pass>", or "disconnect". */
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (nbyte == 0)
        return 0;

    char cmd[WIFI_CMD_MAX];
    size_t len = (nbyte < sizeof(cmd) - 1) ? nbyte : sizeof(cmd) - 1;
    memcpy(cmd, buf, len);
    cmd[len] = '\0';
    /* Trim a trailing newline so callers may or may not send one. */
    if (len > 0 && cmd[len - 1] == '\n')
        cmd[len - 1] = '\0';

    struct wifi_fd_t *f = &d->fds[fd];

    if (strncmp(cmd, "scan", 4) == 0) {
        f->status_done = false;
#ifdef __NuttX__
        WantedFree(f->scan);
        f->scan = scan_collect(d->ifname);
        if (f->scan == NULL)
            return -EIO;
        f->scan_len = strlen(f->scan);
        f->scan_off = 0;
#else
        const char stub[] = "stub-ap 00:00:00:00:00:00 -42\n";
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
        f->status_done = false;
#ifdef __NuttX__
        return wifi_connect(d, ssid, pass) < 0 ? -EIO : (int)nbyte;
#else
        d->state = WIFI_CONNECTED;
        strncpy(d->ssid, ssid, sizeof(d->ssid) - 1);
        d->ssid[sizeof(d->ssid) - 1] = '\0';
        return (int)nbyte;
#endif
    }

    if (strncmp(cmd, "disconnect", 10) == 0) {
        f->status_done = false;
#ifdef __NuttX__
        return wifi_disconnect(d) < 0 ? -EIO : (int)nbyte;
#else
        d->state = WIFI_DISCONNECTED;
        d->ssid[0] = '\0';
        return (int)nbyte;
#endif
    }

    return -EINVAL;
}
