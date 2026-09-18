/* ESP-IDF entry point for the WANTED engine. */

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
extern const uint8_t _binary_flasher_wapp_start[];
extern const uint8_t _binary_flasher_wapp_end[];

/* Generated from the board's WANTED_EXTRA_SEEDS; empty when it names none. */
#include "extra-seeds-decl.inc"

/* True when the registry already holds `ref` ("<name>" or "<name>:<version>").
 */
static bool registryHasRef(const char *ref) {
    reg_entry_t list[REGISTRY_SEED_LIST];
    int n = PlatformRegistryRead(list, REGISTRY_SEED_LIST);

    /* The registry total, capped at what the call filled. */
    if (n > REGISTRY_SEED_LIST)
        n = REGISTRY_SEED_LIST;

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
    ESP_LOGI(TAG, "starting WANTED engine (supervisor: %s)",
             CONFIG_WANTED_SUPERVISOR_IMAGE);
    int ret = WantedStart(_binary_wanted_config_json_start,
                          strlen(_binary_wanted_config_json_start));
    ESP_LOGI(TAG, "WantedStart returned %d", ret);
}
