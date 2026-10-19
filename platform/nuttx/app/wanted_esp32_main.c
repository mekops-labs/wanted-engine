/* SPDX-License-Identifier: Apache-2.0 */

/* ESP32 init shim. The supervisor OCI TAR is bundled into the firmware as a
 * ROMFS image mounted read-only at /rom, since there is no host filesystem to
 * stage it on. This shim owns only that ROMFS. */

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot-romfs.h" /* generated: boot_romfs_img[], boot_romfs_img_len */
#include "wanted_seed.h"

#define ROMFS_MINOR 0
#define ROMFS_SECTSIZE 512
#define ROMFS_DEVPATH "/dev/ram" /* + minor */
#define ROMFS_MOUNTPT "/rom"

/* Writable persistent registry storage, FAT on an external SD card: a separate
 * SPI peripheral whose reads never disable the flash/PSRAM cache. This shim
 * mounts it and chdirs in so the relative registry root persists. */
#define SDCARD_DEVPATH "/dev/mmcsd0"
#define REGISTRY_VOLUME "/sd"

int wanted_main(int argc, char *argv[]);

/* Read all registry images into RAM now, while no wapp and therefore no PSRAM
 * activity is running, which is the only moment an ESP32 flash read is safe.
 * The cache then serves every launch. */
void RegistryCachePreload(void);

#define SEED_DIR                                                               \
    ROMFS_MOUNTPT "/registry" /* /rom/registry (bundled factory wapps) */

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

    /* Mount the SD card holding the writable registry. Reads from here do not
     * disable the flash/PSRAM cache, so they are safe while the runtime holds
     * live PSRAM. */
    if (mount(SDCARD_DEVPATH, REGISTRY_VOLUME, "vfat", 0, NULL) < 0)
        perror("mount " REGISTRY_VOLUME);

    /* Persist the registry on the SD card mounted at /sd. */
    if (chdir(REGISTRY_VOLUME) < 0)
        perror("chdir " REGISTRY_VOLUME);
    else
        WantedSeedRegistry(SEED_DIR);

    /* Cache every registry image in RAM before the supervisor starts (flash
     * reads are only safe while no wapp holds PSRAM). */
    RegistryCachePreload();

    int rc = wanted_main(argc, argv);

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
