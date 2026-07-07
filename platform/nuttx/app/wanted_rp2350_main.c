/* SPDX-License-Identifier: Apache-2.0 */

/* RP2350 init shim.
 *
 * Like the ESP32, the Feather RP2350 has no host filesystem to stage the
 * supervisor image onto (see wanted_sim_main.c for the sim's hostfs case), so
 * the supervisor OCI TAR is bundled into the firmware as a ROMFS image and
 * mounted read-only at /rom; CONFIG_SYSTEM_WANTED_SUPERVISOR_IMAGE points the
 * engine at "/rom/wanted/supervisor.tar". CONFIG_FS_RAMMAP lets the existing
 * mmap-based loader (platform/posix/wapps-image.c) copy the image into the
 * heap.
 *
 * The Feather's console is native USB-CDC, which (unlike a plain UART) is not
 * up at early boot and has no fd bound to it by the generic CONFIG_DEV_CONSOLE
 * path: nsh normally does this itself (apps/nshlib/nsh_usbconsole.c) via
 * BOARDIOC_USBDEV_CDCACM/CONNECT, then waits for a host terminal to actually
 * open the port before dup2'ing it onto fd 0-2. Since this shim replaces
 * nsh_main as the init entrypoint, nsh's own console bring-up never runs, so
 * this shim does the same thing itself; skipping it leaves the engine with no
 * console fds at all (no crash, just total silence).
 *
 * Unlike wanted_esp32_main.c, this shim mounts no writable registry: the M2
 * baseline runs PSRAM-off with no installed wapps, and the QSPI-flash
 * LittleFS registry is a separate, later milestone. Selected via
 * CONFIG_INIT_ENTRYPOINT=wanted_rp2350_main; the NSH built-in entry stays
 * wanted_main. */

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <unistd.h>

#include <nuttx/usb/cdcacm.h>

#include "boot-romfs.h" /* generated: boot_romfs_img[], boot_romfs_img_len */

#define ROMFS_MINOR 0
#define ROMFS_SECTSIZE 512
#define ROMFS_DEVPATH "/dev/ram" /* + minor */
#define ROMFS_MOUNTPT "/rom"

#define CDCACM_DEVMINOR 0
#define CDCACM_DEVPATH "/dev/ttyACM0"

int wanted_main(int argc, char *argv[]);

/* Bring up the USB-CDC console: connect the CDCACM class driver, then block
 * until a host terminal actually opens the port and sends a few carriage
 * returns (same handshake nsh_usbconsole.c uses) before binding it to fd
 * 0-2 - otherwise early engine output races the host terminal attaching and
 * is lost. */
static void bring_up_usb_console(void) {
    struct boardioc_usbdev_ctrl_s ctrl = {
        .usbdev = BOARDIOC_USBDEV_CDCACM,
        .action = BOARDIOC_USBDEV_CONNECT,
        .instance = CDCACM_DEVMINOR,
        .handle = NULL,
    };
    FAR void *handle;
    ctrl.handle = &handle;

    if (boardctl(BOARDIOC_USBDEV_CONTROL, (uintptr_t)&ctrl) < 0) {
        perror("boardctl(USBDEV_CDCACM)");
        return;
    }

    for (;;) {
        int fd;
        do {
            fd = open(CDCACM_DEVPATH, O_RDWR);
            if (fd < 0) {
                sleep(2);
            }
        } while (fd < 0);

        int nlc = 0;
        bool dropped = false;
        while (nlc < 3) {
            char inch = 0;
            ssize_t n = read(fd, &inch, 1);
            if (n == 1 && (inch == '\n' || inch == '\r')) {
                nlc++;
            } else if (n <= 0) {
                close(fd);
                dropped = true;
                break;
            } else {
                nlc = 0;
            }
        }
        if (dropped) {
            continue;
        }

        dup2(fd, 0);
        dup2(fd, 1);
        dup2(fd, 2);
        if (fd > 2) {
            close(fd);
        }
        return;
    }
}

int wanted_rp2350_main(int argc, char *argv[]) {
    struct boardioc_romdisk_s desc = {
        .minor = ROMFS_MINOR,
        .nsectors = (boot_romfs_img_len + ROMFS_SECTSIZE - 1) / ROMFS_SECTSIZE,
        .sectsize = ROMFS_SECTSIZE,
        .image = (FAR uint8_t *)boot_romfs_img,
    };

    if (boardctl(BOARDIOC_ROMDISK, (uintptr_t)&desc) < 0) {
        perror("boardctl(ROMDISK)");
    } else if (mount(ROMFS_DEVPATH "0", ROMFS_MOUNTPT, "romfs", MS_RDONLY,
                     NULL) < 0) {
        perror("mount " ROMFS_MOUNTPT);
    }

    bring_up_usb_console();

    int rc = wanted_main(argc, argv);

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
