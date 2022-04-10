#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <limits.h>

#include <platform.h>

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len)
{
    if (buf == NULL) return -EINVAL;

    srandom((unsigned int)time(NULL));

    for (size_t i = 0; i < buf_len; i++) {
        buf[i] = rand() % UCHAR_MAX;
    }

    return 0;
}
