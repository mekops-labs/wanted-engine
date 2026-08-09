/* SPDX-License-Identifier: Apache-2.0 */

/* blink — toggles a board LED by writing "1"/"0" to /dev/gpio/led/value, whose
 * pin its launch config grants under the name `led`. It runs on any board whose
 * config maps that name onto the right line, and never returns on its own. */

#include <fcntl.h>
#include <unistd.h>

#define GPIO_PATH      "/dev/gpio/led/value"
#define PERIOD_SECONDS 1

int main(void) {
    int fd = open(GPIO_PATH, O_WRONLY);
    if (fd < 0)
        return 1;

    for (;;) {
        if (write(fd, "1", 1) < 0)
            break;
        sleep(PERIOD_SECONDS);
        if (write(fd, "0", 1) < 0)
            break;
        sleep(PERIOD_SECONDS);
    }

    close(fd);
    return 1;
}
