/* SPDX-License-Identifier: Apache-2.0 */

/* B460800/B921600 and TIOCEXCL are Linux extensions to termios. This is the
 * Linux platform layer, which already compiles with _DEFAULT_SOURCE
 * (vfs/vfs-linux.c) — the portable-C rule covers src/, not this seam. */
#define _DEFAULT_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <platform.h>

/* Linux UART backing over termios. The grant's platform key is dev=, a tty
 * path; port= names the port to the wapp. Two opens of one tty both succeed
 * here and then interleave, so this backing takes TIOCEXCL itself. */

#define LINUX_UART_MAX_PORTS 2
#define LINUX_UART_PATH_MAX 64

struct platform_uart_t {
    bool used;
    int fd;
};

static struct platform_uart_t ports[LINUX_UART_MAX_PORTS];

/* Extract dev= from the platform options. Rejects any other key, so a grant
 * meaningless on this target fails the launch. */
static int parseOptions(const char *options, char *path, size_t pathLen) {
    path[0] = '\0';
    if (options == NULL)
        return -EINVAL;

    const char *p = options;
    while (*p != '\0') {
        const char *kv = p;
        while (*p != '\0' && *p != ',')
            p++;
        size_t kvLen = (size_t)(p - kv);
        if (*p == ',')
            p++;
        if (kvLen > 4 && memcmp(kv, "dev=", 4) == 0) {
            size_t vlen = kvLen - 4;
            if (vlen >= pathLen)
                return -EINVAL;
            memcpy(path, kv + 4, vlen);
            path[vlen] = '\0';
        } else {
            return -EINVAL;
        }
    }
    return path[0] != '\0' ? 0 : -EINVAL;
}

/* Map a decimal rate onto a termios speed constant. An unlisted rate is
 * rejected, never rounded to the nearest achievable one. */
static int baudConst(uint32_t baud, speed_t *out) {
    switch (baud) {
    case 9600:
        *out = B9600;
        break;
    case 19200:
        *out = B19200;
        break;
    case 38400:
        *out = B38400;
        break;
    case 57600:
        *out = B57600;
        break;
    case 115200:
        *out = B115200;
        break;
    case 230400:
        *out = B230400;
        break;
    case 460800:
        *out = B460800;
        break;
    case 921600:
        *out = B921600;
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int applyLine(int fd, const plat_uart_cfg_t *cfg) {
    speed_t speed;
    if (baudConst(cfg->baud, &speed) < 0)
        return -EINVAL;

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0)
        return -errno;

    /* Raw mode: no canonical line editing, no echo, no signal generation, no
     * input translation, and no output post-processing. */
    tio.c_iflag &= (tcflag_t) ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR |
                                IGNCR | ICRNL | IXON);
    tio.c_oflag &= (tcflag_t)~OPOST;
    tio.c_lflag &= (tcflag_t) ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cflag &= (tcflag_t)~CSIZE;
    switch (cfg->databits) {
    case 5:
        tio.c_cflag |= CS5;
        break;
    case 6:
        tio.c_cflag |= CS6;
        break;
    case 7:
        tio.c_cflag |= CS7;
        break;
    case 8:
        tio.c_cflag |= CS8;
        break;
    default:
        return -EINVAL;
    }

    if (cfg->parity == 'N') {
        tio.c_cflag &= (tcflag_t) ~(PARENB | PARODD);
    } else {
        tio.c_cflag |= PARENB;
        if (cfg->parity == 'O')
            tio.c_cflag |= PARODD;
        else
            tio.c_cflag &= (tcflag_t)~PARODD;
    }

    if (cfg->stopbits == 2)
        tio.c_cflag |= CSTOPB;
    else
        tio.c_cflag &= (tcflag_t)~CSTOPB;

    tio.c_cflag |= (CLOCAL | CREAD);
    /* Non-blocking at the driver's request: the VFS layer owns the wait. */
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    if (cfsetispeed(&tio, speed) < 0 || cfsetospeed(&tio, speed) < 0)
        return -errno;
    if (tcsetattr(fd, TCSANOW, &tio) < 0)
        return -errno;
    return 0;
}

int PlatformUartOpen(const plat_uart_cfg_t *cfg, platform_uart_t **out) {
    if (cfg == NULL || out == NULL)
        return -EINVAL;

    char path[LINUX_UART_PATH_MAX];
    int rc = parseOptions(cfg->options, path, sizeof(path));
    if (rc < 0)
        return rc;

    struct platform_uart_t *slot = NULL;
    for (int i = 0; i < LINUX_UART_MAX_PORTS; i++) {
        if (!ports[i].used) {
            slot = &ports[i];
            break;
        }
    }
    if (slot == NULL)
        return -ENOSPC;

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -errno;
    if (ioctl(fd, TIOCEXCL) < 0) {
        rc = -errno;
        close(fd);
        return rc;
    }

    rc = applyLine(fd, cfg);
    if (rc < 0) {
        close(fd);
        return rc;
    }

    slot->used = true;
    slot->fd = fd;
    *out = slot;
    return 0;
}

int PlatformUartConfigure(platform_uart_t *u, const plat_uart_cfg_t *cfg) {
    if (u == NULL || !u->used || cfg == NULL)
        return -EINVAL;
    /* Drain what is queued before changing the rate, then drop what arrived
     * under the old settings. */
    if (tcdrain(u->fd) < 0)
        return -errno;
    int rc = applyLine(u->fd, cfg);
    if (rc < 0)
        return rc;
    if (tcflush(u->fd, TCIFLUSH) < 0)
        return -errno;
    return 0;
}

int PlatformUartRead(platform_uart_t *u, void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    ssize_t n = read(u->fd, buf, nbyte);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    return (int)n;
}

int PlatformUartWrite(platform_uart_t *u, const void *buf, size_t nbyte) {
    if (u == NULL || !u->used)
        return -EINVAL;
    ssize_t n = write(u->fd, buf, nbyte);
    if (n < 0)
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -errno;
    return (int)n;
}

void PlatformUartClose(platform_uart_t *u) {
    if (u == NULL || !u->used)
        return;
    close(u->fd);
    memset(u, 0, sizeof(*u));
}
