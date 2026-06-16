/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#include <platform.h>

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) {
    if (buf == NULL)
        return -EINVAL;

    srandom((unsigned int)time(NULL));

    for (size_t i = 0; i < buf_len; i++) {
        buf[i] = random() % UCHAR_MAX;
    }

    return 0;
}
