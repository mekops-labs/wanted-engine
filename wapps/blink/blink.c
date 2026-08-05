/* SPDX-License-Identifier: Apache-2.0 */

/* blink — toggles a board LED through the engine's gpio device node.
 *
 * Its launch config grants the `gpio` driver one pin under the name `led`, for
 * example "pins=led:21:out". The engine mounts that pin at
 * /dev/gpio/led/value; blink drives it by writing "1"/"0" and never returns on
 * its own — the supervisor halts it with a control-plane `stop`.
 *
 * The pin is named, not numbered, so this binary runs on any board whose
 * launch config maps `led` onto the right line. The wapp touches hardware only
 * through the VFS, with no GPIO-specific ABI. */

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
