/* NuttX platform randomness, sourced from the kernel random device. NuttX
 * exposes /dev/urandom via CONFIG_DEV_URANDOM on the sim and ESP32 alike. */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <platform.h>

#define RANDOM_DEVICE "/dev/urandom"

int64_t PlatfromGetRandom(uint8_t *buf, size_t buf_len) {
    if (buf == NULL) {
        return -EINVAL;
    }

    int fd = open(RANDOM_DEVICE, O_RDONLY);
    if (fd < 0) {
        return -errno;
    }

    size_t got = 0;
    while (got < buf_len) {
        ssize_t n = read(fd, buf + got, buf_len - got);
        if (n < 0) {
            int err = errno;
            close(fd);
            return -err;
        }
        if (n == 0) {
            close(fd);
            return -EIO; /* unexpected EOF from a random device */
        }
        got += (size_t)n;
    }

    close(fd);
    return 0;
}
