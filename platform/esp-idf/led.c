/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"

#include <platform.h>

/* ESP-IDF LED backing. A PWM LED takes one LEDC channel, 8-bit at 1 kHz, and
 * its fades run in the LEDC hardware. An on/off LED is a GPIO that a one-shot
 * timer switches at the end of a fade. The address is a decimal GPIO number. */

#define ESP_LED_MAX_LEDS 8
#define ESP_LED_PWM_MAX 255U
#define ESP_LED_PWM_HZ 1000
#define ESP_LED_MODE LEDC_LOW_SPEED_MODE
#define ESP_LED_TIMER LEDC_TIMER_0
#define ESP_LED_US_PER_MS 1000ULL

struct platform_led_t {
    bool used;
    uint8_t mode;
    gpio_num_t pin;
    ledc_channel_t channel;
    volatile unsigned level; /* on/off LEDs */
    esp_timer_handle_t timer;
    unsigned pending; /* the level the timer applies */
};

static struct platform_led_t leds[ESP_LED_MAX_LEDS];
static bool timerReady;

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

static esp_err_t prepareLedc(void) {
    if (timerReady)
        return ESP_OK;
    ledc_timer_config_t timer = {
        .speed_mode = ESP_LED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = ESP_LED_TIMER,
        .freq_hz = ESP_LED_PWM_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err == ESP_OK)
        err = ledc_fade_func_install(0);
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE)
        timerReady = true;
    return timerReady ? ESP_OK : err;
}

static void switchCb(void *arg) {
    struct platform_led_t *l = arg;
    l->level = l->pending;
    gpio_set_level(l->pin, l->pending != 0 ? 1 : 0);
}

static int openPwm(const struct platform_led_t *l) {
    if (prepareLedc() != ESP_OK)
        return -EIO;
    ledc_channel_config_t ch = {
        .gpio_num = l->pin,
        .speed_mode = ESP_LED_MODE,
        .channel = l->channel,
        .timer_sel = ESP_LED_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch) == ESP_OK ? 0 : -EIO;
}

static int openOnOff(struct platform_led_t *l) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << (unsigned)l->pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_timer_create_args_t args = {.callback = switchCb, .arg = l};
    if (gpio_config(&io) != ESP_OK ||
        esp_timer_create(&args, &l->timer) != ESP_OK)
        return -EIO;
    gpio_set_level(l->pin, 0);
    return 0;
}

int PlatformLedOpen(const plat_led_cfg_t *cfg, platform_led_t **out) {
    if (cfg == NULL || out == NULL)
        return -EINVAL;
    int pin = parsePin(cfg->address);
    if (pin < 0 || !GPIO_IS_VALID_OUTPUT_GPIO((gpio_num_t)pin))
        return -EINVAL;

    struct platform_led_t *slot = NULL;
    int index = -1;
    for (int i = 0; i < ESP_LED_MAX_LEDS; i++) {
        if (leds[i].used && leds[i].pin == (gpio_num_t)pin)
            return -EBUSY;
        if (!leds[i].used && slot == NULL) {
            slot = &leds[i];
            index = i;
        }
    }
    if (slot == NULL || index >= LEDC_CHANNEL_MAX)
        return -ENOSPC;

    memset(slot, 0, sizeof(*slot));
    slot->mode = cfg->mode;
    slot->pin = (gpio_num_t)pin;
    slot->channel = (ledc_channel_t)index;
    int rc = cfg->mode == PLAT_LED_MODE_PWM ? openPwm(slot) : openOnOff(slot);
    if (rc < 0)
        return rc;

    slot->used = true;
    *out = slot;
    return 0;
}

unsigned PlatformLedMax(const platform_led_t *l) {
    return (l != NULL && l->mode == PLAT_LED_MODE_PWM) ? ESP_LED_PWM_MAX : 1U;
}

bool PlatformLedHwFade(const platform_led_t *l) {
    return l != NULL && l->mode == PLAT_LED_MODE_PWM;
}

unsigned PlatformLedGet(const platform_led_t *l) {
    if (l == NULL || !l->used)
        return 0;
    if (l->mode == PLAT_LED_MODE_PWM)
        return ledc_get_duty(ESP_LED_MODE, l->channel);
    return l->level;
}

static int setPwm(struct platform_led_t *l, unsigned level) {
    ledc_fade_stop(ESP_LED_MODE, l->channel);
    if (ledc_set_duty(ESP_LED_MODE, l->channel, level) != ESP_OK ||
        ledc_update_duty(ESP_LED_MODE, l->channel) != ESP_OK)
        return -EIO;
    return 0;
}

int PlatformLedSet(platform_led_t *l, unsigned level) {
    if (l == NULL || !l->used || level > PlatformLedMax(l))
        return -EINVAL;
    if (l->mode == PLAT_LED_MODE_PWM)
        return setPwm(l, level);

    esp_timer_stop(l->timer);
    l->level = level;
    return gpio_set_level(l->pin, level != 0 ? 1 : 0) == ESP_OK ? 0 : -EIO;
}

int PlatformLedFade(platform_led_t *l, unsigned level, unsigned ms) {
    if (l == NULL || !l->used || level > PlatformLedMax(l))
        return -EINVAL;

    if (l->mode != PLAT_LED_MODE_PWM) {
        esp_timer_stop(l->timer);
        l->pending = level;
        return esp_timer_start_once(l->timer, ms * ESP_LED_US_PER_MS) == ESP_OK
                   ? 0
                   : -EIO;
    }

    ledc_fade_stop(ESP_LED_MODE, l->channel);
    if (level == ledc_get_duty(ESP_LED_MODE, l->channel))
        return 0;
    if (ledc_set_fade_with_time(ESP_LED_MODE, l->channel, level, ms) !=
            ESP_OK ||
        ledc_fade_start(ESP_LED_MODE, l->channel, LEDC_FADE_NO_WAIT) != ESP_OK)
        return setPwm(l, level);
    return 0;
}

void PlatformLedClose(platform_led_t *l) {
    if (l == NULL || !l->used)
        return;
    if (l->mode == PLAT_LED_MODE_PWM) {
        ledc_fade_stop(ESP_LED_MODE, l->channel);
        ledc_stop(ESP_LED_MODE, l->channel, 0);
    } else {
        esp_timer_stop(l->timer);
        esp_timer_delete(l->timer);
    }
    gpio_reset_pin(l->pin);
    gpio_set_direction(l->pin, GPIO_MODE_OUTPUT);
    gpio_set_level(l->pin, 0);
    memset(l, 0, sizeof(*l));
}
