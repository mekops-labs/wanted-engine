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

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "boot-romfs.h" /* generated: boot_romfs_img[], boot_romfs_img_len */

#define ROMFS_MINOR 0
#define ROMFS_SECTSIZE 512
#define ROMFS_DEVPATH "/dev/ram" /* + minor */
#define ROMFS_MOUNTPT "/rom"

/* Writable persistent registry storage. The board late-init (esp32_bringup ->
 * esp32_spiflash_init) mounts a LittleFS over the SPI-flash storage MTD here;
 * chdir into it so the engine's relative REGISTRY_ROOT ("./registry") persists
 * on flash across reboots. Installed wapps live here; the supervisor image is
 * the read-only ROMFS at /rom. */
#define REGISTRY_VOLUME "/data"

int wanted_main(int argc, char *argv[]);

/* Read all registry images into RAM now, while no wapp (hence no WAMR/PSRAM
 * activity) is running. On ESP32 an SPI-flash read returns corrupt data once a
 * wapp holds live PSRAM, so the engine must not read image files off flash at
 * launch time; the cache (platform/nuttx/api/registry.c) serves launches. */
void RegistryCachePreload(void);

#define SEED_DIR                                                               \
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
 * into the writable flash registry, skipping ones already installed (O_EXCL).
 * Lets a freshly-flashed board start its bundled wapps with no network; on
 * later boots the persisted copies win and nothing is re-seeded. */
static void seed_registry(void) {
    DIR *d = opendir(SEED_DIR);
    if (!d)
        return; /* no factory bundle */
    mkdir(REGISTRY_DIR, 0755);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char src[256], dst[256];
        snprintf(src, sizeof(src), "%s/%s", SEED_DIR, e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", REGISTRY_DIR, e->d_name);
        seed_copy(src, dst);
    }
    closedir(d);
}

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

    /* Persist the registry on the flash LittleFS the board mounted at /data. */
    if (chdir(REGISTRY_VOLUME) < 0)
        perror("chdir " REGISTRY_VOLUME);
    else
        seed_registry();

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
