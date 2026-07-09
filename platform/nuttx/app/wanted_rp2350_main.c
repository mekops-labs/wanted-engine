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
 * Either way, the CDC-ACM class driver is connected regardless of which
 * transport is the console: with UART0 as console, USB-CDC is still brought
 * up (just not bound to fd 0-2), so a wapp can be granted a `serial://`
 * socket onto CDCACM_DEVPATH - e.g. Sheriff's Deputy uplink, with UART0
 * reserved for the debug console (see plans/wanted-sheriff-deputy-uart-
 * transport.md in the mekops-kb).
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

#include <platform.h>
#include <wanted.h>

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

/* Sheriff demo support (plans/wanted-sheriff-deputy-uart-transport.md in the
 * mekops-kb): when CONFIG_SYSTEM_WANTED_BOOT_ROMFS_SUPERVISOR is "sheriff",
 * this shim provisions Sheriff's identity and passes its own launch config
 * instead of falling through to wanted_main()'s generic minimal default
 * (which has no mounts/sockets at all - fine for wsh, since wsh's console is
 * wired up separately above, but Sheriff needs a `platform` mount and a
 * `manager` socket to even start).
 *
 * Identity provisioning mirrors the ESP-IDF port's one-off hardware-
 * validation run (see the mekops-kb plan): Sheriff's on-disk identity format
 * has no interactive way to write it (wsh's `write` command can't carry raw
 * CBOR), so this shim writes the bytes directly with plain POSIX calls
 * against the LittleFS volume - the same primitives seed_copy() above
 * already uses, no VfsPlatformFsInit/WASI-preopen machinery needed since
 * this code runs as plain NuttX C, not inside a wapp's sandbox.
 *
 * The pubkey below is a fixed demo key (Ed25519 pubkey for the all-0x11
 * 32-byte seed already used as SHERIFF_E2E_SEED's default in
 * wapps/sheriff/test/e2e-deputy.sh) - matches a Deputy instance started with
 * `--signing-seed <the same all-0x11 hex>`, not a real provisioning flow. */
#define SHERIFF_IDENTITY_DIR "sheriff/identity" /* under REGISTRY_VOLUME */
#define SHERIFF_DEVICE_ID "rp2350-01"

static const uint8_t sheriff_demo_pubkey[32] = {
    0xd0, 0x4a, 0xb2, 0x32, 0x74, 0x2b, 0xb4, 0xab, 0x3a, 0x13, 0x68,
    0xbd, 0x46, 0x15, 0xe4, 0xe6, 0xd0, 0x22, 0x4a, 0xb7, 0x1a, 0x01,
    0x6b, 0xaf, 0x85, 0x20, 0xa3, 0x32, 0xc9, 0x77, 0x87, 0x37,
};

/* CBOR array(1)[map(2){0: key_id(1), 1: pubkey(bstr32)}] - matches
 * identity.zig's on-disk format and harness.sh's Python-built envelope. */
static void write_marshal_pubkeys(const char *path) {
    static const uint8_t header[] = {0x81, 0xA2, 0x00, 0x01, 0x01, 0x58, 0x20};
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        DEBUG_TRACE("write %s failed: %s", path, strerror(errno));
        return;
    }
    write(fd, header, sizeof(header));
    write(fd, sheriff_demo_pubkey, sizeof(sheriff_demo_pubkey));
    close(fd);
}

static void write_device_id(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        DEBUG_TRACE("write %s failed: %s", path, strerror(errno));
        return;
    }
    write(fd, SHERIFF_DEVICE_ID, strlen(SHERIFF_DEVICE_ID));
    close(fd);
}

/* Idempotent to re-run every boot: O_TRUNC overwrites in place, no stale
 * leftovers from a prior demo key. Run after chdir(REGISTRY_VOLUME), so
 * these paths are relative to it, matching seed_registry()'s convention. */
static void provision_sheriff_identity(void) {
    mkdir("sheriff", 0755);
    mkdir(SHERIFF_IDENTITY_DIR, 0755);
    write_marshal_pubkeys(SHERIFF_IDENTITY_DIR "/marshal_pubkeys.cbor");
    write_device_id(SHERIFF_IDENTITY_DIR "/device_id");
    DEBUG_TRACE("sheriff identity provisioned under %s/" SHERIFF_IDENTITY_DIR,
                REGISTRY_VOLUME);
}

/* `platform` mount: options carries `src=<real path>`, since the wapp-visible
 * path (Sheriff's hardcoded ROOT_PATH, storage.zig) must stay "/var/lib/
 * sheriff" while the real backing directory lives under the flash-MTD
 * registry volume (REGISTRY_VOLUME "/" SHERIFF_IDENTITY_DIR's parent, i.e.
 * REGISTRY_VOLUME "/sheriff") - there is no real "/var" on this board.
 * "manager" is the one socket name Sheriff's engine-owned uplink looks for
 * (sheriff/src/main.zig: MANAGER_SOCKET = "/net/manager") - the address here
 * is what actually varies per deployment: serial:///dev/ttyACM0 on the
 * RP2350 (no network stack; see plans/wanted-sheriff-deputy-uart-transport.md)
 * versus tcp://... on Linux/ESP-IDF. */
#define SHERIFF_CFG                                                            \
    "{\"system\":{\"privileged\":true},"                                       \
    "\"supervisor\":{\"params\":{"                                             \
    "\"console\":{\"in\":{\"name\":\"platform\"},"                             \
    "\"out\":{\"name\":\"platform\"},"                                         \
    "\"err\":{\"name\":\"platform\"}},"                                        \
    "\"drivers\":[{\"name\":\"wanted\"},{\"name\":\"sha256\"},"                \
    "{\"name\":\"ed25519\"},{\"name\":\"inflate\"}],"                          \
    "\"mounts\":[{\"name\":\"platform\",\"path\":\"/var/lib/sheriff\","        \
    "\"options\":\"src=" REGISTRY_VOLUME "/sheriff\"}],"                       \
    "\"sockets\":[{\"name\":\"manager\",\"address\":"                          \
    "\"serial://" CDCACM_DEVPATH "\"}]}}}"

/* Connect the CDCACM class driver so CDCACM_DEVPATH exists and is openable.
 * Needed either way: as the console's own transport (below), or - when the
 * console is on UART0 instead - so a wapp can still be granted a `serial://`
 * socket onto it (e.g. Sheriff's Deputy uplink over USB-CDC, with UART0
 * reserved for the debug console; see plans/wanted-sheriff-deputy-uart-
 * transport.md). Idempotent to call once; NuttX doesn't expose a "already
 * connected" query, so this shim only ever calls it the one time below. */
static void connect_usb_cdcacm(void) {
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
    }
}

#ifndef CONFIG_UART0_SERIAL_CONSOLE
/* Bring up the USB-CDC console: connect the CDCACM class driver, then block
 * until a host terminal actually opens the port and sends a few carriage
 * returns (same handshake nsh_usbconsole.c uses) before binding it to fd
 * 0-2 - otherwise early engine output races the host terminal attaching and
 * is lost. */
static void bring_up_usb_console(void) {
    connect_usb_cdcacm();

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
    /* Grab the PSRAM pool before anything else (littlefs/ROMFS mount,
     * seed_registry file I/O) touches the shared heap and fragments its one
     * big PSRAM free node - see PlatformExtramEarlyInit's doc comment and
     * the M4 status note in plans/wanted-sheriff-deputy-uart-transport.md.
     * Tried moving this to run after seed_registry()/provision_sheriff_
     * identity() instead (hypothesis: keep PSRAM "clean" through those flash
     * writes, dodging the cache-coherency corruption below) - ruled out on
     * hardware: extram_init()'s probes then fail outright (g_extram stays
     * NULL, same fragmentation this early call exists to prevent), just
     * trading the corruption hang for the original "allocate linear memory
     * failed" failure. Keep this call first. */
    PlatformExtramEarlyInit();

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

#ifdef CONFIG_UART0_SERIAL_CONSOLE
    /* Console is already on UART0 (CONFIG_DEV_CONSOLE bound fd 0-2 before
     * this shim ran) - still connect CDC-ACM so a wapp can be granted a
     * `serial://` socket onto it. */
    connect_usb_cdcacm();
#else
    bring_up_usb_console();
#endif

    /* Persist the registry on the LittleFS volume board bring-up already
     * mounted at REGISTRY_VOLUME. Done after the console is up so any
     * failure here is visible instead of silently lost. */
    bool sheriffDemo = false;
    if (chdir(REGISTRY_VOLUME) < 0) {
        perror("chdir " REGISTRY_VOLUME);
    } else {
        DEBUG_TRACE("chdir %s ok", REGISTRY_VOLUME);
        seed_registry();
        sheriffDemo =
            strcmp(CONFIG_SYSTEM_WANTED_BOOT_ROMFS_SUPERVISOR, "sheriff") == 0;
        if (sheriffDemo) {
            provision_sheriff_identity();
        }
    }

    /* All flash-program calls above are done; repair the heap header before
     * the engine's first real allocation - see PlatformExtramRepairHeader's
     * doc comment in platform.h. */
    PlatformExtramRepairHeader();

    int rc;
    if (sheriffDemo) {
        rc = WantedStart(SHERIFF_CFG, strlen(SHERIFF_CFG));
    } else {
        rc = wanted_main(argc, argv);
    }

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
