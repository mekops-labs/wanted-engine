/* SPDX-License-Identifier: Apache-2.0 */

/* Linux platform randomness, read from the kernel random device. */

#include <errno.h>
#include <stdio.h>

#include <platform.h>

#define RANDOM_DEVICE "/dev/urandom"

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) {
    if (buf == NULL)
        return -EINVAL;

    FILE *f = fopen(RANDOM_DEVICE, "rb");
    if (f == NULL)
        return errno != 0 ? -errno : -EIO;

    size_t got = fread(buf, 1, buf_len, f);
    fclose(f);
    return got == buf_len ? (int64_t)got : -EIO;
}
