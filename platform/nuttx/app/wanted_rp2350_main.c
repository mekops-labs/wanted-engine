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
 * Console: the Feather's default `wanted` defconfig uses native USB-CDC,
 * which (unlike a plain UART) is not up at early boot and has no fd bound to
 * it by the generic CONFIG_DEV_CONSOLE path: nsh normally brings it up itself
 * (apps/nshlib/nsh_usbconsole.c) via BOARDIOC_USBDEV_CDCACM/CONNECT, then
 * waits for a host terminal to actually open the port before dup2'ing it onto
 * fd 0-2. Since this shim replaces nsh_main as the init entrypoint, nsh's own
 * console bring-up never runs, so this shim does the same thing itself;
 * skipping it leaves the engine with no console fds at all (no crash, just
 * total silence).
 *
 * A UART0 console is also supported, selected purely by CONFIG_
 * UART0_SERIAL_CONSOLE (no WANTED-specific Kconfig needed): with that set,
 * CONFIG_DEV_CONSOLE already binds fd 0-2 to UART0 before this shim runs
 * (UART needs no enumeration/host handshake the way USB-CDC does), so
 * bring_up_usb_console() is skipped entirely. Useful as a higher-throughput,
 * more reliable transport for -DDEBUG=1 engine tracing than USB-CDC, whose
 * throughput/capture reliability struggles under that volume - build with
 * CONFIG_RP23XX_UART0=y + CONFIG_UART0_SERIAL_CONSOLE=y + CONFIG_DEV_CONSOLE=y
 * and CDCACM_CONSOLE/NSH_USBCONSOLE unset, then read/write the Debug Probe's
 * UART bridge tty (a second, separate /dev/ttyACMx from the board's own
 * USB-CDC - identify both by USB descriptor, not device number). Start any
 * capture *before* triggering reset/flash: boot console output is a one-time
 * burst with no ongoing heartbeat once wsh reaches its idle prompt, so a
 * reader that attaches even slightly late looks identical to "nothing was
 * ever sent".
 *
 * Unlike wanted_esp32_main.c, this shim mounts no writable registry itself:
 * the LittleFS volume over the internal-flash MTD region (CONFIG_
 * RP23XX_FLASH_MTD) is already mounted by board bring-up
 * (rp23xx_common_bringup.c), before this shim ever runs. This shim only
 * chdir's into it and seeds any bundled factory wapps, mirroring the ESP32
 * shim's SD-card handling minus the mount. Selected via
 * CONFIG_INIT_ENTRYPOINT=wanted_rp2350_main; the NSH built-in entry stays
 * wanted_main. */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nuttx/usb/cdcacm.h>

#include "boot-romfs.h" /* generated: boot_romfs_img[], boot_romfs_img_len */
#include "debug_trace.h"

#define ROMFS_MINOR 0
#define ROMFS_SECTSIZE 512
#define ROMFS_DEVPATH "/dev/ram" /* + minor */
#define ROMFS_MOUNTPT "/rom"

#define CDCACM_DEVMINOR 0
#define CDCACM_DEVPATH "/dev/ttyACM0"

/* Writable persistent registry storage: the LittleFS volume board bring-up
 * already mounted over the internal-flash MTD region at
 * CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT. Unlike the ESP32 (SD card, kept off
 * internal SPI flash to dodge a flash/PSRAM cache-coherency bug), this M3
 * baseline is PSRAM-off, so no such coexistence issue applies yet - flag for
 * revisiting once PSRAM lands (see the flash-mtd driver plan's Risk 2). */
#define REGISTRY_VOLUME CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT

int wanted_main(int argc, char *argv[]);

#define SEED_DIR                                                              \
    ROMFS_MOUNTPT "/registry"   /* /rom/registry (bundled factory wapps) */
#define REGISTRY_DIR "registry" /* relative to REGISTRY_VOLUME (chdir'd) */
#define SEED_COPY_BUF 1024

/* Copy one factory image from the read-only boot ROMFS into the writable
 * registry. Best-effort: a failure just means that image is not installed. */
static void seed_copy(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0)
        return;
    int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        close(in);
        return;
    }
    char buf[SEED_COPY_BUF];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n)
            break;
    }
    close(in);
    close(out);
}

/* First-boot factory seed: copy any /rom/registry/*.wapp the firmware bundles
 * into the writable registry, skipping ones already installed (O_EXCL). Lets
 * a freshly-flashed board start its bundled wapps with no network; on later
 * boots the persisted copies win and nothing is re-seeded. */
static void seed_registry(void) {
    DIR *d = opendir(SEED_DIR);
    if (!d) {
        DEBUG_TRACE("opendir(%s) failed: %s", SEED_DIR, strerror(errno));
        return; /* no factory bundle */
    }
    mkdir(REGISTRY_DIR, 0755);
    const struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        DEBUG_TRACE("seeding %s", e->d_name);
        char src[256], dst[256];
        snprintf(src, sizeof(src), "%s/%s", SEED_DIR, e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", REGISTRY_DIR, e->d_name);
        seed_copy(src, dst);
    }
    closedir(d);
}

#ifndef CONFIG_UART0_SERIAL_CONSOLE
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
#endif /* !CONFIG_UART0_SERIAL_CONSOLE */

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

#ifndef CONFIG_UART0_SERIAL_CONSOLE
    bring_up_usb_console();
#endif

    /* Persist the registry on the LittleFS volume board bring-up already
     * mounted at REGISTRY_VOLUME. Done after the console is up so any
     * failure here is visible instead of silently lost. */
    if (chdir(REGISTRY_VOLUME) < 0) {
        perror("chdir " REGISTRY_VOLUME);
    } else {
        DEBUG_TRACE("chdir %s ok", REGISTRY_VOLUME);
        seed_registry();
    }

    int rc = wanted_main(argc, argv);

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
