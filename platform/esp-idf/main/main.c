//
//  Wasm3 - high performance WebAssembly interpreter written in C.
//
//  Copyright © 2019 Steven Massey, Volodymyr Shymanskyy.
//  All rights reserved.
//

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_vfs_dev.h"
#include "sdkconfig.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wanted.h>

#define TAG "main"
#define FATAL(msg, ...)                                                        \
    {                                                                          \
        printf("Fatal: " msg "\n", ##__VA_ARGS__);                             \
        return;                                                                \
    }
#define STR(...) #__VA_ARGS__

char *cfg = STR({
    "system" : {"defaultWapps" : [ "a", "bb", "ccc" ]},
    "supervisor" : {
        "action" : "start",
        "params" : {
            "name" : "supervisor",
            "console" : {
                "in" : {"name" : "platform"},
                "out" : {"name" : "platform"},
                "err" : {"name" : "platform"}
            },
            "drivers" : [
                {"name" : "rom", "path" : "/rom"},
                {"name" : "platform", "path" : "/mnt", "options" : "/spiffs/"},
                {"name" : "virt", "path" : "/net"}, {
                    "name" : "socket",
                    "path" : "/net/s",
                    "options" : "t localhost 8888"
                },
                {
                    "name" : "socket",
                    "path" : "/net/ss",
                    "options" : "T localhost 8889"
                },
                {"name" : "virt", "path" : "/d"}, {
                    "name" : "9p",
                    "path" : "/d/b",
                    "options" : "tcp!localhost!5640"
                },
                {"name" : "wanted", "path" : "/w"}, {
                    "name" : "config",
                    "path" : "/config",
                    "options" : "{\"config_file\":\"/mnt/config.json\"}"
                }
            ]
        }
    }
});

void initializeStorage() {
    esp_vfs_spiffs_conf_t conf = {.base_path = "/spiffs",
                                  .partition_label = NULL,
                                  .max_files = 5,
                                  .format_if_mount_failed = true};

    // Use settings defined above to initialize and mount SPIFFS filesystem.
    // Note: esp_vfs_spiffs_register is an all-in-one convenience function.
    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)",
                     esp_err_to_name(ret));
        }
        return;
    }

#ifdef CONFIG_SPIFFS_CHECK_ON_START
    ESP_LOGI(TAG, "Performing SPIFFS_check().");
    ret = esp_spiffs_check(conf.partition_label);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
        return;
    } else {
        ESP_LOGI(TAG, "SPIFFS_check() successful");
    }
#endif

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(conf.partition_label, &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to get SPIFFS partition information (%s). Formatting...",
            esp_err_to_name(ret));
        esp_spiffs_format(conf.partition_label);
        return;
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }

    // Check consistency of reported partiton size info.
    if (used > total) {
        ESP_LOGW(TAG, "Number of used bytes cannot be larger than total. "
                      "Performing SPIFFS_check().");
        ret = esp_spiffs_check(conf.partition_label);
        // Could be also used to mend broken files, to clean unreferenced pages,
        // etc. More info at
        // https://github.com/pellepl/spiffs/wiki/FAQ#powerlosses-contd-when-should-i-run-spiffs_check
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "SPIFFS_check() failed (%s)", esp_err_to_name(ret));
            return;
        } else {
            ESP_LOGI(TAG, "SPIFFS_check() successful");
        }
    }
}

void configure_stdin_stdout(void) {
    // Initialize VFS & UART so we can use std::cout/cin
    setvbuf(stdin, NULL, _IONBF, 0);
    /* Install UART driver for interrupt-driven reads and writes */
    ESP_ERROR_CHECK(uart_driver_install(
        (uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0));
    /* Tell VFS to use UART driver */

    esp_vfs_dev_uart_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    esp_vfs_dev_uart_port_set_rx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                              ESP_LINE_ENDINGS_CR);
    /* Move the caret to the beginning of the next line on '\n' */
    esp_vfs_dev_uart_port_set_tx_line_endings(CONFIG_ESP_CONSOLE_UART_NUM,
                                              ESP_LINE_ENDINGS_CRLF);
}

void app_main(void) {
    configure_stdin_stdout();

    // esp_vfs_dev_uart_use_nonblocking(CONFIG_ESP_CONSOLE_UART_NUM);

    printf("\nWanted on " CONFIG_IDF_TARGET ", build " __DATE__ " " __TIME__
           "\n");

    initializeStorage();
    ESP_LOGE(TAG, "free stack: %d", uxTaskGetStackHighWaterMark(NULL));

    clock_t start = clock();
    int ret = WantedStart(cfg, strlen(cfg));
    if (ret < 0) {
        errno = -ret;
        perror("wanted error");
    }

    printf("\nAll wapps ended, done...\n");

    clock_t end = clock();

    printf("Elapsed: %ld ms\n", (end - start) * 1000 / CLOCKS_PER_SEC);

    sleep(3);
    printf("Restarting...\n\n\n");
    esp_restart();
}
