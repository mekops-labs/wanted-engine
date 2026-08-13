/* SPDX-License-Identifier: Apache-2.0 */

/* The reset reason the ROM and the panic handler leave behind, as a token a
 * reader can match on. It says whether the previous boot ended in a watchdog,
 * a panic, a commanded reboot or a power cycle. */

#include <stddef.h>
#include <string.h>

#include <esp_system.h>
#include <platform.h>

size_t PlatformResetReason(char *buf, size_t len) {
    const char *token;

    if (buf == NULL || len == 0) {
        return 0;
    }

    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
        token = "poweron";
        break;
    case ESP_RST_SW:
        token = "sw";
        break;
    case ESP_RST_PANIC:
        token = "panic";
        break;
    case ESP_RST_TASK_WDT:
        token = "task_wdt";
        break;
    case ESP_RST_INT_WDT:
        token = "int_wdt";
        break;
    case ESP_RST_WDT:
        token = "wdt";
        break;
    case ESP_RST_BROWNOUT:
        token = "brownout";
        break;
    case ESP_RST_DEEPSLEEP:
        token = "deepsleep";
        break;
    case ESP_RST_EXT:
        token = "ext";
        break;
    default:
        token = "unknown";
        break;
    }

    size_t n = strlen(token);
    if (n + 1 > len) {
        return 0;
    }
    memcpy(buf, token, n + 1);
    return n;
}
