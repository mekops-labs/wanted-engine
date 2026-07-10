/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF WiFi station driver. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define TAG "vfs-wifi"

#include <debug_trace.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted_malloc.h>

static const char id[] = {'W', 'i', 'f', 'i'};

#define WIFI_MAX_FDS 2   /* concurrent opens of the node */
#define WIFI_SSID_MAX 33 /* 32 + NUL */
#define WIFI_CMD_MAX 128 /* longest accepted command line */

struct wifi_fd_t {
    bool used;
    /* A status read is a one-shot-per-state latch, not one-shot-per-fd: it
     * re-arms whenever the connection state changes, not only on a fresh
     * write. A poll loop that holds this fd open across an async connect
     * (write "connect", then repeated reads with no intervening write —
     * exactly what wapps/wifi-connect does) must still observe the
     * disconnected->connected transition; a plain one-shot-per-write latch
     * (matching the NuttX platform, where connect blocks synchronously so
     * the very next read already reflects the outcome) went permanently
     * silent after the first poll here, confirmed on-target: the radio
     * associated and got a DHCP lease, but wifi-connect's own retry loop
     * never saw it and timed out. */
    bool status_done;
    bool last_connected;
    char *scan; /* heap scan-result text, drained by reads */
    size_t scan_len;
    size_t scan_off;
};

struct vfs_driver_ctx_t {
    struct wifi_fd_t fds[WIFI_MAX_FDS];
};

static bool g_wifiStarted;
static bool g_wifiConnected;
static char g_wifiSsid[WIFI_SSID_MAX];
static char g_wifiIp[16] = "0.0.0.0";

static void wifiEventHandler(void *arg, esp_event_base_t base, int32_t evId,
                             void *data) {
    (void)arg;
    if (base == WIFI_EVENT && evId == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *evt =
            (const wifi_event_sta_disconnected_t *)data;
        ESP_LOGI(TAG, "STA_DISCONNECTED reason=%d", evt ? evt->reason : -1);
        g_wifiConnected = false;
        strncpy(g_wifiIp, "0.0.0.0", sizeof(g_wifiIp) - 1);
        g_wifiIp[sizeof(g_wifiIp) - 1] = '\0';
    } else if (base == WIFI_EVENT) {
        ESP_LOGI(TAG, "WIFI_EVENT id=%d", (int)evId);
    }
}

/* Signature must match esp_event_handler_t exactly (esp_event_handler_
 * instance_register's expected type); const-qualifying data would mismatch
 * it and force a cast at the registration call site. */
static void ipEventHandler(void *arg, esp_event_base_t base, int32_t evId,
                           /* cppcheck-suppress constParameterCallback */
                           void *data) {
    (void)arg;
    if (base == IP_EVENT && evId == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = (const ip_event_got_ip_t *)data;
        esp_ip4addr_ntoa(&evt->ip_info.ip, g_wifiIp, sizeof(g_wifiIp));
        g_wifiConnected = true;
    }
}

/* One-time WiFi station bring-up: NVS (esp_wifi_init needs it for
 * calibration data), the default event loop, the STA netif, the driver, and
 * the event handlers. Idempotent (guarded by g_wifiStarted) so every wapp
 * granted the wifi driver finds the radio ready — matching the NuttX
 * platform's ifup-on-init contract. */
static bool wifiEnsureStarted(void) {
    if (g_wifiStarted)
        return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() != ESP_OK)
            return false;
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
        return false;

    if (esp_event_loop_create_default() != ESP_OK)
        return false;
    if (esp_netif_create_default_wifi_sta() == NULL)
        return false;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK)
        return false;

    esp_event_handler_instance_t wifiHandle, ipHandle;
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                            wifiEventHandler, NULL,
                                            &wifiHandle) != ESP_OK)
        return false;
    if (esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                            ipEventHandler, NULL,
                                            &ipHandle) != ESP_OK)
        return false;

    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
        return false;
    if (esp_wifi_start() != ESP_OK)
        return false;

    g_wifiStarted = true;
    return true;
}

/* Run a blocking scan and return its results as one malloc'd text block of
 * "<ssid> <bssid> <rssi>\n" lines (caller owns it). NULL on failure. */
static char *scanCollect(void) {
    wifi_scan_config_t scanCfg;
    memset(&scanCfg, 0, sizeof(scanCfg));
    scanCfg.show_hidden = true;

    esp_err_t startErr = esp_wifi_scan_start(&scanCfg, true);
    ESP_LOGI(TAG, "scan_start -> %s", esp_err_to_name(startErr));
    if (startErr != ESP_OK)
        return NULL;

    uint16_t num = 0;
    esp_err_t numErr = esp_wifi_scan_get_ap_num(&num);
    ESP_LOGI(TAG, "scan_get_ap_num -> %s num=%u", esp_err_to_name(numErr),
             (unsigned)num);
    if (numErr != ESP_OK)
        return NULL;

    wifi_ap_record_t *records = NULL;
    if (num > 0) {
        records = (wifi_ap_record_t *)WantedMalloc(sizeof(*records) * num);
        if (records == NULL)
            return NULL;
        uint16_t got = num;
        if (esp_wifi_scan_get_ap_records(&got, records) != ESP_OK) {
            WantedFree(records);
            return NULL;
        }
        num = got;
    }

    char line[WIFI_SSID_MAX + 64];
    size_t total = 1; /* trailing NUL */
    for (uint16_t i = 0; i < num; i++) {
        int n = snprintf(
            line, sizeof(line), "%s %02x:%02x:%02x:%02x:%02x:%02x %d\n",
            (const char *)records[i].ssid, records[i].bssid[0],
            records[i].bssid[1], records[i].bssid[2], records[i].bssid[3],
            records[i].bssid[4], records[i].bssid[5], (int)records[i].rssi);
        if (n > 0)
            total += (size_t)n;
    }

    char *out = (char *)WantedMalloc(total);
    if (out != NULL) {
        size_t off = 0;
        for (uint16_t i = 0; i < num; i++) {
            int n = snprintf(
                out + off, total - off, "%s %02x:%02x:%02x:%02x:%02x:%02x %d\n",
                (const char *)records[i].ssid, records[i].bssid[0],
                records[i].bssid[1], records[i].bssid[2], records[i].bssid[3],
                records[i].bssid[4], records[i].bssid[5], (int)records[i].rssi);
            if (n > 0)
                off += (size_t)n;
        }
        out[off] = '\0';
    }

    WantedFree(records);
    return out;
}

/* Configure the target AP and kick off an asynchronous association; the
 * WIFI_EVENT/IP_EVENT handlers above update the module-global connection
 * state as the radio actually associates and leases an address. An empty
 * pass configures an open network. */
static int wifiConnect(const char *ssid, const char *pass) {
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    strncpy((char *)conf.sta.ssid, ssid, sizeof(conf.sta.ssid) - 1);
    strncpy((char *)conf.sta.password, pass, sizeof(conf.sta.password) - 1);
    conf.sta.threshold.authmode =
        (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    /* WPA2/WPA3-transition APs commonly expect a PMF-capable client even
     * when not requiring it; leaving pmf_cfg zeroed (not capable) made a
     * real AP reject the very first 802.11 open-auth frame (AUTH_EXPIRE,
     * confirmed on-target) before the WPA2 handshake ever started. */
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;

    if (esp_wifi_set_config(WIFI_IF_STA, &conf) != ESP_OK)
        return -1;

    strncpy(g_wifiSsid, ssid, sizeof(g_wifiSsid) - 1);
    g_wifiSsid[sizeof(g_wifiSsid) - 1] = '\0';

    return (esp_wifi_connect() == ESP_OK) ? 0 : -1;
}

static int wifiDisconnectNow(void) {
    return (esp_wifi_disconnect() == ESP_OK) ? 0 : -1;
}

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);

vfs_driver_t *VfsWifiInit(const wapp_t *wapp, const char *options) {
    (void)wapp;
    (void)options;

    vfs_driver_t *driver = (vfs_driver_t *)WantedMalloc(sizeof(vfs_driver_t));
    if (driver == NULL) {
        DEBUG_TRACE("can't allocate memory");
        return NULL;
    }

    struct vfs_driver_ctx_t *ctx = (struct vfs_driver_ctx_t *)WantedMalloc(
        sizeof(struct vfs_driver_ctx_t));
    if (ctx == NULL) {
        DEBUG_TRACE("can't allocate memory");
        WantedFree(driver);
        return NULL;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(driver, 0, sizeof(*driver));

    wifiEnsureStarted();

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

    if (f->status_done && g_wifiConnected == f->last_connected)
        return 0;

    char line[WIFI_SSID_MAX + 32];
    int n;
    if (g_wifiConnected)
        n = snprintf(line, sizeof(line), "connected %s %s\n", g_wifiSsid,
                     g_wifiIp);
    else
        n = snprintf(line, sizeof(line), "disconnected\n");
    if (n < 0)
        return -EIO;

    size_t len = (size_t)n;
    size_t out = (nbyte < len) ? nbyte : len;
    memcpy(buf, line, out);
    f->status_done = true;
    f->last_connected = g_wifiConnected;
    return (int)out;
}

/* write: a text command — "scan", "connect <ssid> <pass>", or "disconnect". */
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;
    if (nbyte == 0)
        return 0;
    if (!wifiEnsureStarted())
        return -EIO;

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
        WantedFree(f->scan);
        f->scan = scanCollect();
        if (f->scan == NULL)
            return -EIO;
        f->scan_len = strlen(f->scan);
        f->scan_off = 0;
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
        return (wifiConnect(ssid, pass) < 0) ? -EIO : (int)nbyte;
    }

    if (strncmp(cmd, "disconnect", 10) == 0) {
        f->status_done = false;
        return (wifiDisconnectNow() < 0) ? -EIO : (int)nbyte;
    }

    return -EINVAL;
}
