/* ESP-IDF entry point for the WANTED engine. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"

#include <platform.h>
#include <vfs-drivers.h>
#include <vfs.h>
#include <wanted-autoconf.h>
#include <wanted.h>
#if CONFIG_WANTED_ESP_IDF_WIFI_BOOT_JOIN
#include <wifi-bringup.h>
#endif

#define TAG "wanted"
#define LITTLEFS_PARTITION_LABEL "persist"
/* Registry entries read when checking whether a seed ref is already present. */
#define REGISTRY_SEED_LIST 16

static bool mountLittleFs(void) {
    esp_vfs_littlefs_conf_t conf = {
        .base_path = PlatformVolumeRoot(),
        .partition_label = LITTLEFS_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info(LITTLEFS_PARTITION_LABEL, &total, &used);
    ESP_LOGI(TAG, "littlefs mounted at %s: used=%u total=%u bytes",
             PlatformVolumeRoot(), (unsigned)used, (unsigned)total);
    return true;
}

/* Smoke-test fixtures linked via EMBED_FILES. */
extern const uint8_t _binary_wifi_connect_wapp_start[];
extern const uint8_t _binary_wifi_connect_wapp_end[];
extern const uint8_t _binary_flasher_wapp_start[];
extern const uint8_t _binary_flasher_wapp_end[];

/* Generated from the board's WANTED_EXTRA_SEEDS; empty when it names none. */
#include "extra-seeds-decl.inc"

/* True when the registry already holds `ref` ("<name>" or "<name>:<version>").
 */
static bool registryHasRef(const char *ref) {
    reg_entry_t list[REGISTRY_SEED_LIST];
    int n = PlatformRegistryRead(list, REGISTRY_SEED_LIST);

    const char *colon = strchr(ref, ':');
    size_t nameLen = (colon != NULL) ? (size_t)(colon - ref) : strlen(ref);
    const char *version = (colon != NULL) ? colon + 1 : "";

    for (int i = 0; i < n; i++) {
        if (strlen(list[i].name) != nameLen)
            continue;
        if (strncmp(list[i].name, ref, nameLen) != 0)
            continue;
        if (strcmp(list[i].version, version) == 0)
            return true;
    }
    return false;
}

/* Factory-seeds a wapp into the flash registry, once. Leaves a ref the
 * registry already holds, so an image installed over a seeded ref survives the
 * next boot. */
static void seedWapp(const char *ref, const uint8_t *start,
                     const uint8_t *end) {
    /* Marked whether or not it is written: after the first boot the image is
     * already there, and a ref the firmware owns is still not a supervisor's
     * to reclaim. */
    PlatformRegistryMarkSeeded(ref);

    if (registryHasRef(ref)) {
        ESP_LOGI(TAG, "seed: %s present, keeping the installed image", ref);
        return;
    }

    size_t len = (size_t)(end - start);
    int w = PlatformRegistryWrite(START_WRITE, ref, start, len);
    int fin = PlatformRegistryWrite(FINISH_WRITE, NULL, NULL, 0);
    ESP_LOGI(TAG, "seed: %s (%u bytes) -> write=%d finish=%d", ref,
             (unsigned)len, w, fin);
}

#if CONFIG_WANTED_ESP_IDF_WIFI_BOOT_JOIN
#define WIFI_CONF_PATH "/data/wifi.conf"
#define WIFI_SSID_MAX 33 /* 32 + NUL */
#define WIFI_PASS_MAX 64 /* 63 + NUL */
#define WIFI_JOIN_TIMEOUT_S 20

/* One line, newline-stripped, from the console. Empty on EOF. */
static void readConsoleLine(const char *prompt, char *out, size_t outLen) {
    printf("%s", prompt);
    fflush(stdout);
    out[0] = '\0';
    if (fgets(out, (int)outLen, stdin) == NULL) {
        out[0] = '\0';
        return;
    }
    char *nl = strpbrk(out, "\r\n");
    if (nl != NULL)
        *nl = '\0';
}

/* Two lines: SSID, then passphrase. False when the file is absent or short. */
static bool readWifiConf(char *ssid, size_t ssidLen, char *pass,
                         size_t passLen) {
    FILE *f = fopen(WIFI_CONF_PATH, "r");
    if (f == NULL)
        return false;

    bool ok = fgets(ssid, (int)ssidLen, f) != NULL;
    if (ok && fgets(pass, (int)passLen, f) == NULL)
        pass[0] = '\0';
    fclose(f);
    if (!ok)
        return false;

    char *nl = strpbrk(ssid, "\r\n");
    if (nl != NULL)
        *nl = '\0';
    nl = strpbrk(pass, "\r\n");
    if (nl != NULL)
        *nl = '\0';
    return ssid[0] != '\0';
}

static void writeWifiConf(const char *ssid, const char *pass) {
    FILE *f = fopen(WIFI_CONF_PATH, "w");
    if (f == NULL) {
        ESP_LOGW(TAG, "wifi: cannot save credentials to " WIFI_CONF_PATH);
        return;
    }
    fprintf(f, "%s\n%s\n", ssid, pass);
    fclose(f);
    ESP_LOGI(TAG, "wifi: credentials saved; later boots need no operator");
}

/* Join Wi-Fi before the supervisor starts, for a supervisor whose control plane
 * is on the network. Stored credentials make a reboot unattended -- which an
 * OTA depends on, since the update reboots the board. */
static void wifiBringup(void) {
    char ssid[WIFI_SSID_MAX] = {0};
    char pass[WIFI_PASS_MAX] = {0};

    bool stored = readWifiConf(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!stored) {
        readConsoleLine("wifi ssid: ", ssid, sizeof(ssid));
        if (ssid[0] == '\0') {
            ESP_LOGI(TAG, "wifi: no ssid given, starting without the network");
            return;
        }
        readConsoleLine("wifi passphrase: ", pass, sizeof(pass));
    }

    /* The SSID is deliberately not logged: a boot log is copied into issues and
     * pastes, and the network a device joins does not need to travel with it.
     */
    int rc = EspWifiBringup(ssid, pass, WIFI_JOIN_TIMEOUT_S);
    ESP_LOGI(TAG, "wifi: join -> rc=%d", rc);
    if (rc == 0 && !stored)
        writeWifiConf(ssid, pass);

    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
}
#endif /* CONFIG_WANTED_ESP_IDF_WIFI_BOOT_JOIN */

/* Route the console VFS through the interrupt-driven driver so read(stdin)
 * blocks; the default console is non-blocking and a shell's getline() then
 * spins forever. The peripheral differs by board, the requirement does not. */
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
static void consoleUseBlockingDriver(void) {
    usb_serial_jtag_driver_config_t cfg =
        USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    ESP_LOGI(TAG, "console: usb_serial_jtag_driver_install -> %s",
             err == ESP_OK ? "OK" : esp_err_to_name(err));
    usb_serial_jtag_vfs_use_driver();
}
#elif CONFIG_ESP_CONSOLE_UART_DEFAULT
static void consoleUseBlockingDriver(void) {
    esp_err_t err =
        uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "console: uart_driver_install -> %s",
             err == ESP_OK ? "OK" : esp_err_to_name(err));
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
}
#endif

/* Launch config, embedded by main/CMakeLists.txt from the configured JSON.
 * EMBED_TXTFILES NUL-terminates it, so it is already a C string. */
extern const char _binary_wanted_config_json_start[];

void app_main(void) {
    ESP_LOGI(TAG, "WANTED engine — ESP-IDF platform bring-up");
    ESP_LOGI(TAG, "platform: %s", PlatformName());

    size_t used = 0, total = 0;
    PlatformMemoryStats(&used, &total);
    ESP_LOGI(TAG, "memory: used=%u total=%u bytes", (unsigned)used,
             (unsigned)total);

    if (mountLittleFs()) {
        seedWapp("wifi-connect", _binary_wifi_connect_wapp_start,
                 _binary_wifi_connect_wapp_end);
        /* Versioned by the tree it was built from, so a newer flasher
         * installs alongside this one and a launch config selects which
         * runs. */
        seedWapp(WANTED_FLASHER_REF, _binary_flasher_wapp_start,
                 _binary_flasher_wapp_end);
#include "extra-seeds-call.inc"
    }

    /* Starts lwIP's tcpip thread; required before any socket() call. */
    esp_err_t netifErr = esp_netif_init();
    ESP_LOGI(TAG, "netif: init -> %s", netifErr == ESP_OK ? "OK" : "FAIL");

    int otaRc = PlatformOtaInit();
    ESP_LOGI(TAG, "ota: init -> rc=%d", otaRc);

    PlatformSetProcessArgs(0, NULL);
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_UART_DEFAULT
    consoleUseBlockingDriver();
#endif
#if CONFIG_WANTED_ESP_IDF_WIFI_BOOT_JOIN
    /* After the blocking console driver: the credential prompt reads a line,
     * and a non-blocking stdin returns EOF instead of waiting for one. */
    wifiBringup();
#endif
    ESP_LOGI(TAG, "starting WANTED engine (supervisor: %s)",
             CONFIG_WANTED_SUPERVISOR_IMAGE);
    int ret = WantedStart(_binary_wanted_config_json_start,
                          strlen(_binary_wanted_config_json_start));
    ESP_LOGI(TAG, "WantedStart returned %d", ret);
}
