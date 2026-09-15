/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF WiFi driver: /dev/wifi/{status,scan,ctl}. Station and access-point
 * control for a single radio; the driver owns reconnection and derives AP
 * credentials from the hardware serial, so nothing credential-shaped crosses
 * the wapp boundary. */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#define TAG "vfs-wifi"

#include <debug_trace.h>
#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-api.h>
#include <wanted-hmac.h>
#include <wanted_malloc.h>
#include <wifi-bringup.h>

static const char id[] = {'W', 'i', 'f', 'i'};

#define WIFI_MAX_FDS 4   /* concurrent opens across status/scan/ctl */
#define WIFI_SSID_MAX 33 /* 32 + NUL */
#define WIFI_PASS_MAX 64 /* 63 + NUL */
#define WIFI_CMD_MAX 128 /* longest accepted command line */

#define WIFI_NODE_ROOT 0
#define WIFI_NODE_STATUS 1
#define WIFI_NODE_SCAN 2
#define WIFI_NODE_CTL 3

/* AP credentials never come from a wapp. SSID carries no secret (a compiled-in
 * prefix plus the low 24 bits of the serial, in hex); the passphrase is
 * HMAC-SHA256(serial, label), rendered as its first 16 hex digits. */
#define WIFI_AP_SSID_PREFIX "wanted-"
#define WIFI_AP_SSID_SUFFIX_LEN 6 /* hex digits: low 24 bits of the serial */
#define WIFI_AP_PASS_LABEL "wanted-ap-passphrase-v1"
#define WIFI_AP_PASS_HEX_LEN 16 /* first 8 HMAC bytes, rendered as hex */
#define WIFI_AP_CHANNEL 1
#define WIFI_AP_MAX_CONN 4

#define WIFI_RECONNECT_MIN_DELAY_MS 1000
#define WIFI_RECONNECT_MAX_DELAY_MS 30000

struct wifi_fd_t {
    bool used;
    uint8_t node;
    /* A status read latches per descriptor, re-arming whenever the radio's
     * state changes — g_wifiGen bumps on every such change, so comparing
     * against the last-seen value is the whole check. */
    uint32_t seenGen;
    size_t scan_off; /* this descriptor's read position in g_scanBuf */
};

struct vfs_driver_ctx_t {
    struct wifi_fd_t fds[WIFI_MAX_FDS];
};

/* Scan results are module-level, like every other radio state here: `ctl`
 * (which triggers a scan) and `scan` (which reads it) are different
 * descriptors, so the result cannot live on either one's own fd. */
static char *g_scanBuf;
static size_t g_scanLen;

/* Module-level, not per-descriptor: a fire-and-forget wapp raises a link or an
 * AP and exits, and the link must survive it. */
static bool g_wifiStarted;
static bool g_wifiConnected;
static bool g_hasIntent; /* a station "connect" intent is stored */
static bool g_apMode;
static char g_staSsid[WIFI_SSID_MAX];
static char g_apSsid[WIFI_SSID_MAX];
static char g_wifiIp[16] = "0.0.0.0";
static uint32_t g_wifiGen = 1; /* never 0, so a fresh fd's seenGen=0 misses */
static uint32_t g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
static esp_timer_handle_t g_reconnectTimer;

static void reconnectTimerCb(void *arg) {
    (void)arg;
    if (g_hasIntent && !g_apMode && !g_wifiConnected)
        esp_wifi_connect();
}

/* Unbounded retry with a capped backoff: `disconnected` means no intent
 * stored, never "gave up". Resets to the floor on a fresh connect or a
 * successful association. */
static void scheduleReconnect(void) {
    esp_timer_stop(g_reconnectTimer); /* no-op if not running */
    esp_timer_start_once(g_reconnectTimer, (uint64_t)g_reconnectDelayMs * 1000);
    uint32_t next = g_reconnectDelayMs * 2;
    g_reconnectDelayMs = (next > WIFI_RECONNECT_MAX_DELAY_MS)
                             ? WIFI_RECONNECT_MAX_DELAY_MS
                             : next;
}

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
        g_wifiGen++;
        if (g_hasIntent && !g_apMode)
            scheduleReconnect();
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
        g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
        g_wifiGen++;
    }
}

/* One-time WiFi bring-up: NVS (needed for calibration data), the default
 * event loop, both the STA and AP netifs, the driver, the event handlers and
 * the reconnect timer. Idempotent, so every wapp granted the driver finds the
 * radio ready. */
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
    if (esp_netif_create_default_wifi_ap() == NULL)
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

    const esp_timer_create_args_t timerArgs = {
        .callback = &reconnectTimerCb,
        .name = "wifi-reconnect",
    };
    if (esp_timer_create(&timerArgs, &g_reconnectTimer) != ESP_OK)
        return false;

    /* Credentials stay in RAM: a board's flash outlives the session that
     * typed them, and nothing here needs them again after a reboot. */
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK)
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

/* Configure the target AP and start an asynchronous association; the event
 * handlers above update the connection state as the radio associates and
 * leases an address, and re-associate it on an unsolicited drop. Replaces
 * whatever intent — station or AP — was previously stored. */
static int setStationIntent(const char *ssid, const char *pass) {
    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    strncpy((char *)conf.sta.ssid, ssid, sizeof(conf.sta.ssid) - 1);
    strncpy((char *)conf.sta.password, pass, sizeof(conf.sta.password) - 1);
    conf.sta.threshold.authmode =
        (pass[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    /* A WPA2/WPA3-transition AP commonly expects a PMF-capable client even
     * when it does not require one; a zeroed pmf_cfg gets the first open-auth
     * frame rejected with AUTH_EXPIRE. */
    conf.sta.pmf_cfg.capable = true;
    conf.sta.pmf_cfg.required = false;

    if (g_apMode) {
        esp_wifi_disconnect(); /* harmless if the AP interface is idle */
        esp_wifi_set_mode(WIFI_MODE_STA);
        g_apMode = false;
    }
    esp_timer_stop(g_reconnectTimer);

    if (esp_wifi_set_config(WIFI_IF_STA, &conf) != ESP_OK)
        return -EIO;

    strncpy(g_staSsid, ssid, sizeof(g_staSsid) - 1);
    g_staSsid[sizeof(g_staSsid) - 1] = '\0';
    g_hasIntent = true;
    g_reconnectDelayMs = WIFI_RECONNECT_MIN_DELAY_MS;
    g_wifiGen++;

    return (esp_wifi_connect() == ESP_OK) ? 0 : -EIO;
}

/* Clears the stored station intent: no further auto-reconnect, and the radio
 * drops whatever association it holds. Idempotent. */
static void stopStation(void) {
    g_hasIntent = false;
    esp_timer_stop(g_reconnectTimer);
    esp_wifi_disconnect();
    g_wifiConnected = false;
    g_wifiGen++;
}

/* Count the visible APs and say whether `ssid` is among them, without logging
 * any network's name: a boot log travels, and the answer a bring-up needs is
 * whether the radio can see the target at all. */
static void reportVisibility(const char *ssid) {
    char *aps = scanCollect();
    if (aps == NULL) {
        ESP_LOGW(TAG, "bringup: scan failed");
        return;
    }

    size_t count = 0;
    for (const char *p = aps; *p != '\0'; p++) {
        if (*p == '\n')
            count++;
    }

    bool found = false;
    size_t len = strlen(ssid);
    for (const char *line = aps; line != NULL && *line != '\0';) {
        if (strncmp(line, ssid, len) == 0 && line[len] == ' ') {
            found = true;
            break;
        }
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }

    ESP_LOGI(TAG, "bringup: %u APs visible, target present: %s",
             (unsigned)count, found ? "yes" : "no");
    /* Only when the target is missing, and only then: the operator needs to see
     * what the radio can reach — a 5 GHz-only SSID is invisible here, and the
     * failure otherwise looks identical to a wrong passphrase. */
    if (!found)
        ESP_LOGI(TAG, "bringup: visible:\n%s", aps);
    WantedFree(aps);
}

int EspWifiBringup(const char *ssid, const char *pass, int timeoutSec) {
    if (ssid == NULL || ssid[0] == '\0')
        return -EINVAL;
    if (!wifiEnsureStarted())
        return -EIO;

    reportVisibility(ssid);

    if (setStationIntent(ssid, pass != NULL ? pass : "") < 0)
        return -EIO;

    /* The driver retries on its own now; this loop only waits for the first
     * association within the caller's budget. */
    for (int tenths = 0; tenths < timeoutSec * 10; tenths++) {
        if (g_wifiConnected)
            return 0;
        usleep(100 * 1000);
    }
    return -ETIMEDOUT;
}

/* Fills `ssidOut`/`passOut` from the hardware serial: SSID carries a
 * compiled-in prefix plus the low 24 bits of the serial (already hex text, so
 * its last WIFI_AP_SSID_SUFFIX_LEN characters are exactly that); the
 * passphrase is HMAC-SHA256(serial, label), truncated to its first
 * WIFI_AP_PASS_HEX_LEN hex digits. -ENODEV where the platform has no serial to
 * derive from — no fallback to a fixed passphrase. */
static int deriveApCredentials(char ssidOut[WIFI_SSID_MAX],
                               char passOut[WIFI_AP_PASS_HEX_LEN + 1]) {
    char serial[PLATFORM_SERIAL_MAX_LEN + 1];
    int n = PlatformSerialNumber(serial, sizeof(serial));
    if (n <= 0)
        return -ENODEV;

    const char *suffix = (n >= WIFI_AP_SSID_SUFFIX_LEN)
                             ? serial + (n - WIFI_AP_SSID_SUFFIX_LEN)
                             : serial;
    /* Explicit precision, not a bare %s: the suffix pointer's static bound is
     * PLATFORM_SERIAL_MAX_LEN as far as the compiler can tell, which is wider
     * than ssidOut — bound the read to what actually follows the prefix. */
    snprintf(ssidOut, WIFI_SSID_MAX, "%s%.*s", WIFI_AP_SSID_PREFIX,
             WIFI_AP_SSID_SUFFIX_LEN, suffix);

    static const char hexDigits[] = "0123456789abcdef";
    uint8_t digest[PLATFORM_SHA256_DIGEST_LEN];
    int rc = WantedHmacSha256((const uint8_t *)serial, (size_t)n,
                              (const uint8_t *)WIFI_AP_PASS_LABEL,
                              strlen(WIFI_AP_PASS_LABEL), digest);
    if (rc < 0)
        return rc;

    for (int i = 0; i < WIFI_AP_PASS_HEX_LEN / 2; i++) {
        passOut[i * 2] = hexDigits[digest[i] >> 4];
        passOut[i * 2 + 1] = hexDigits[digest[i] & 0x0f];
    }
    passOut[WIFI_AP_PASS_HEX_LEN] = '\0';
    return 0;
}

/* Hosts an AP with driver-derived credentials, replacing whatever intent —
 * station or AP — was previously stored. -ENODEV where the platform reports
 * no serial to derive a passphrase from. */
static int setApIntent(void) {
    char ssid[WIFI_SSID_MAX];
    char pass[WIFI_AP_PASS_HEX_LEN + 1];
    int rc = deriveApCredentials(ssid, pass);
    if (rc < 0)
        return rc;

    g_hasIntent = false;
    esp_timer_stop(g_reconnectTimer);
    if (g_wifiConnected)
        esp_wifi_disconnect();

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK)
        return -EIO;

    wifi_config_t conf;
    memset(&conf, 0, sizeof(conf));
    strncpy((char *)conf.ap.ssid, ssid, sizeof(conf.ap.ssid) - 1);
    conf.ap.ssid_len = (uint8_t)strlen(ssid);
    strncpy((char *)conf.ap.password, pass, sizeof(conf.ap.password) - 1);
    conf.ap.authmode = WIFI_AUTH_WPA2_PSK;
    conf.ap.channel = WIFI_AP_CHANNEL;
    conf.ap.max_connection = WIFI_AP_MAX_CONN;

    if (esp_wifi_set_config(WIFI_IF_AP, &conf) != ESP_OK)
        return -EIO;

    strncpy(g_apSsid, ssid, sizeof(g_apSsid) - 1);
    g_apSsid[sizeof(g_apSsid) - 1] = '\0';
    g_apMode = true;
    g_wifiConnected = false;
    g_wifiGen++;
    return 0;
}

/* Idempotent: stops a running AP and returns the radio to station mode. A
 * successful station association does not implicitly call this — only
 * ap_stop, or a fresh connect, brings the AP down. */
static void stopAp(void) {
    if (!g_apMode)
        return;
    esp_wifi_set_mode(WIFI_MODE_STA);
    g_apMode = false;
    g_wifiGen++;
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

static int _Destroy(struct vfs_driver_t *d);
static int _Open(vfs_driver_ctx_t d, const char *path, vfs_oflags_t flags);
static int _Close(vfs_driver_ctx_t d, int fd);
static int _Stat(vfs_driver_ctx_t d, int fd, vfs_stat_t *stat);
static int _Read(vfs_driver_ctx_t d, int fd, void *buf, size_t nbyte);
static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte);
static int _ReadDir(vfs_driver_ctx_t d, int fd, void *buf, size_t bufLen,
                    uint64_t *cookie, size_t *bufUsed);

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
    /* g_scanBuf is module-level, like the connection state — it outlives this
     * driver instance and is freed only when a fresh scan replaces it. */
    WantedFree(d->ctx);
    WantedFree(d);
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

/* One line, no trailing detail beyond what MDR-0047 lists: disconnected,
 * connecting (covers an initial association and a driver-initiated retry
 * alike), connected <ssid> <ip>, or ap <ssid>. */
static size_t renderStatus(char *line, size_t lineLen) {
    int n;
    if (g_apMode)
        n = snprintf(line, lineLen, "ap %s\n", g_apSsid);
    else if (g_wifiConnected)
        n = snprintf(line, lineLen, "connected %s %s\n", g_staSsid, g_wifiIp);
    else if (g_hasIntent)
        n = snprintf(line, lineLen, "connecting\n");
    else
        n = snprintf(line, lineLen, "disconnected\n");
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
        if (g_scanBuf == NULL || f->scan_off >= g_scanLen)
            return 0; /* no scan run yet, or this descriptor drained it */
        size_t left = g_scanLen - f->scan_off;
        size_t n = (nbyte < left) ? nbyte : left;
        memcpy(buf, g_scanBuf + f->scan_off, n);
        f->scan_off += n;
        return (int)n;
    }

    /* WIFI_NODE_STATUS */
    if (f->seenGen == g_wifiGen)
        return 0;
    char line[WIFI_SSID_MAX + 32];
    size_t len = renderStatus(line, sizeof(line));
    size_t n = (nbyte < len) ? nbyte : len;
    memcpy(buf, line, n);
    f->seenGen = g_wifiGen;
    return (int)n;
}

static int _Write(vfs_driver_ctx_t d, int fd, const void *buf, size_t nbyte) {
    if (fd < 0 || fd >= WIFI_MAX_FDS || !d->fds[fd].used)
        return -EBADF;

    const struct wifi_fd_t *f = &d->fds[fd];
    if (f->node == WIFI_NODE_ROOT)
        return -EISDIR;
    if (f->node == WIFI_NODE_STATUS)
        return -EPERM; /* read-only */
    if (nbyte == 0)
        return 0;
    if (!wifiEnsureStarted())
        return -EIO;

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
        WantedFree(g_scanBuf);
        g_scanBuf = scanCollect();
        if (g_scanBuf == NULL) {
            g_scanLen = 0;
            return -EIO;
        }
        g_scanLen = strlen(g_scanBuf);
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
        return (setStationIntent(ssid, pass) < 0) ? -EIO : (int)nbyte;
    }

    if (strcmp(cmd, "disconnect") == 0) {
        stopStation();
        return (int)nbyte;
    }

    if (strcmp(cmd, "ap_start") == 0) {
        int rc = setApIntent();
        if (rc == -ENODEV)
            return -ENODEV;
        return (rc < 0) ? -EIO : (int)nbyte;
    }

    if (strcmp(cmd, "ap_stop") == 0) {
        stopAp();
        return (int)nbyte;
    }

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
