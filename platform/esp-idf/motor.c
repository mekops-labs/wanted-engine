/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include <platform.h>

/* ESP-IDF motor backing: a GPIO that a one-shot timer steps through the
 * pattern's segments. The address is a decimal GPIO number. */

#define ESP_MOTOR_MAX 2
#define ESP_MOTOR_SEGMENTS_MAX 8
#define ESP_MOTOR_US_PER_MS 1000ULL

struct platform_motor_t {
    bool used;
    gpio_num_t pin;
    esp_timer_handle_t timer;
    portMUX_TYPE mux;
    uint16_t seg[ESP_MOTOR_SEGMENTS_MAX];
    size_t count;
    size_t next;
    volatile bool busy;
};

static struct platform_motor_t motors[ESP_MOTOR_MAX];

/* Decimal GPIO number, or -1 when the address is not one. */
static int parsePin(const char *address) {
    if (address == NULL || address[0] == '\0')
        return -1;
    int n = 0;
    for (const char *p = address; *p != '\0'; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        n = n * 10 + (*p - '0');
    }
    return n;
}

/* Starts the next segment, or ends the pattern with the motor off. The pin
 * and the timer change under the lock, so a stop or a close that takes it
 * after this step sees the motor off and the timer to cancel. */
static void step(void *arg) {
    struct platform_motor_t *m = arg;

    portENTER_CRITICAL(&m->mux);
    if (m->used) {
        bool more = m->next < m->count;
        int level = 0;
        if (more) {
            level = (m->next % 2 == 0) ? 1 : 0;
            unsigned ms = m->seg[m->next++];
            esp_timer_start_once(m->timer, ms * ESP_MOTOR_US_PER_MS);
        } else {
            m->busy = false;
        }
        gpio_set_level(m->pin, level);
    }
    portEXIT_CRITICAL(&m->mux);
}

int PlatformMotorOpen(const char *address, platform_motor_t **out) {
    if (out == NULL)
        return -EINVAL;
    int pin = parsePin(address);
    if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin))
        return -EINVAL;

    struct platform_motor_t *slot = NULL;
    for (int i = 0; i < ESP_MOTOR_MAX; i++) {
        if (motors[i].used && motors[i].pin == (gpio_num_t)pin)
            return -EBUSY;
        if (!motors[i].used && slot == NULL)
            slot = &motors[i];
    }
    if (slot == NULL)
        return -ENOSPC;

    memset(slot, 0, sizeof(*slot));
    slot->pin = (gpio_num_t)pin;
    portMUX_INITIALIZE(&slot->mux);

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_timer_create_args_t args = {.callback = step, .arg = slot};
    if (gpio_config(&io) != ESP_OK ||
        esp_timer_create(&args, &slot->timer) != ESP_OK)
        return -EIO;
    gpio_set_level(slot->pin, 0);

    slot->used = true;
    *out = slot;
    return 0;
}

int PlatformMotorPattern(platform_motor_t *m, const uint16_t *ms, size_t n) {
    if (m == NULL || !m->used || ms == NULL || n == 0 ||
        n > ESP_MOTOR_SEGMENTS_MAX)
        return -EINVAL;

    esp_timer_stop(m->timer);
    portENTER_CRITICAL(&m->mux);
    memcpy(m->seg, ms, n * sizeof(ms[0]));
    m->count = n;
    m->next = 0;
    m->busy = true;
    portEXIT_CRITICAL(&m->mux);
    step(m);
    return 0;
}

void PlatformMotorStop(platform_motor_t *m) {
    if (m == NULL || !m->used)
        return;
    portENTER_CRITICAL(&m->mux);
    m->count = 0;
    m->next = 0;
    m->busy = false;
    gpio_set_level(m->pin, 0);
    portEXIT_CRITICAL(&m->mux);
    esp_timer_stop(m->timer);
}

bool PlatformMotorBusy(const platform_motor_t *m) {
    return m != NULL && m->used && m->busy;
}

/* A step already running sees the slot unused and leaves the pin alone. */
void PlatformMotorClose(platform_motor_t *m) {
    if (m == NULL || !m->used)
        return;
    PlatformMotorStop(m);
    portENTER_CRITICAL(&m->mux);
    m->used = false;
    portEXIT_CRITICAL(&m->mux);
    esp_timer_stop(m->timer);
    esp_timer_delete(m->timer);
    gpio_reset_pin(m->pin);
    gpio_set_direction(m->pin, GPIO_MODE_OUTPUT);
    gpio_set_level(m->pin, 0);
}
