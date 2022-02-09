#include <sys/random.h>
#include <errno.h>

#include <platform.h>

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len)
{
    int64_t ret = getrandom(buf, buf_len, 0);
    if (ret < 0) {
        return -errno;
    }

    return ret;
}
