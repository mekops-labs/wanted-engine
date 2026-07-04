/* SPDX-License-Identifier: Apache-2.0 */

/* ESP-IDF platform randomness from the hardware RNG. esp_fill_random draws from
 * the RNG seeded by the SAR ADC / RF noise; it cannot fail. */

#include <errno.h>

#include <platform.h>

#include "esp_random.h"

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) {
    if (buf == NULL)
        return -EINVAL;
    esp_fill_random(buf, buf_len);
    return 0;
}
