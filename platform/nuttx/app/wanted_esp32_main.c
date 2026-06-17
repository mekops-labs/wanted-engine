/* SPDX-License-Identifier: Apache-2.0 */

/* ESP32 init shim.
 *
 * On ESP32 there is no host filesystem to stage the supervisor image onto (the
 * sim uses hostfs; see wanted_sim_main.c). The supervisor OCI TAR is therefore
 * bundled into the firmware as a ROMFS image and mounted read-only at /rom;
 * CONFIG_SYSTEM_WANTED_SUPERVISOR_IMAGE points the engine at
 * "/rom/wanted/supervisor.tar". CONFIG_FS_RAMMAP lets the existing mmap-based
 * loader (platform/posix/wapps-image.c) copy the image into the (PSRAM) heap —
 * the documented RAM-copy fallback for targets without execute-in-place flash.
 *
 * Installed wapps live on a writable LittleFS registry mounted by the board
 * bring-up; this shim only owns the read-only supervisor ROMFS. Selected via
 * CONFIG_INIT_ENTRYPOINT=wanted_esp32_main; the NSH built-in entry stays
 * wanted_main. */

#include <stdio.h>
#include <sys/boardctl.h>
#include <sys/mount.h>

#include "boot-romfs.h" /* generated: boot_romfs_img[], boot_romfs_img_len */

#define ROMFS_MINOR 0
#define ROMFS_SECTSIZE 512
#define ROMFS_DEVPATH "/dev/ram" /* + minor */
#define ROMFS_MOUNTPT "/rom"

int wanted_main(int argc, char *argv[]);

int wanted_esp32_main(int argc, char *argv[]) {
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

    int rc = wanted_main(argc, argv);

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
