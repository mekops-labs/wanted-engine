/* SPDX-License-Identifier: Apache-2.0 */

/* RP2350 init shim.
 *
 * The supervisor OCI TAR is bundled into the firmware as a ROMFS image,
 * mounted read-only at /rom. CONFIG_SYSTEM_WANTED_SUPERVISOR_IMAGE points
 * the engine at "/rom/wanted/supervisor.tar"; CONFIG_FS_RAMMAP lets the
 * mmap-based loader (platform/posix/wapps-image.c) copy it into the heap.
 *
 * Console: this shim replaces nsh_main as the init entrypoint, so nsh's own
 * USB-CDC console bring-up never runs; bring_up_usb_console() does it here
 * instead. CONFIG_UART0_SERIAL_CONSOLE selects UART0 instead, whose fd 0-2
 * binding is already done by CONFIG_DEV_CONSOLE before this shim runs. The
 * CDC-ACM class driver is connected either way, so a wapp can be granted a
 * `serial://` socket onto CDCACM_DEVPATH.
 *
 * The registry's LittleFS volume (CONFIG_RP23XX_FLASH_MTD) is mounted by
 * board bring-up (rp23xx_common_bringup.c) before this shim runs; it only
 * chdir's into it and seeds bundled factory wapps. Entry point is selected
 * via CONFIG_INIT_ENTRYPOINT=wanted_rp2350_main; the NSH built-in entry
 * stays wanted_main. */

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
 * CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT. */
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

/* When CONFIG_SYSTEM_WANTED_BOOT_ROMFS_SUPERVISOR is "sheriff", provisions
 * Sheriff's identity and passes a launch config with a `platform` mount and
 * `manager` socket, instead of wanted_main()'s minimal default.
 *
 * Identity is written directly via POSIX calls against the LittleFS volume,
 * since there is no interactive way to write raw CBOR.
 *
 * The pubkey below is a fixed demo key: the Ed25519 pubkey for the all-0x11
 * 32-byte seed used as SHERIFF_E2E_SEED's default in
 * wapps/sheriff/test/e2e-deputy.sh, matching a Deputy instance started with
 * `--signing-seed <the same all-0x11 hex>`. */
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

/* Idempotent: O_TRUNC overwrites in place. Paths are relative to
 * REGISTRY_VOLUME (chdir'd by the caller). */
static void provision_sheriff_identity(void) {
    mkdir("sheriff", 0755);
    mkdir(SHERIFF_IDENTITY_DIR, 0755);
    write_marshal_pubkeys(SHERIFF_IDENTITY_DIR "/marshal_pubkeys.cbor");
    write_device_id(SHERIFF_IDENTITY_DIR "/device_id");
    DEBUG_TRACE("sheriff identity provisioned under %s/" SHERIFF_IDENTITY_DIR,
                REGISTRY_VOLUME);
}

/* `platform` mount: options carries `src=<real path>`, since the wapp-visible
 * path (Sheriff's hardcoded ROOT_PATH, storage.zig) stays "/var/lib/sheriff"
 * while the real backing directory is REGISTRY_VOLUME "/sheriff" - there is
 * no real "/var" on this board. "manager" is the socket name Sheriff's
 * engine-owned uplink looks for (sheriff/src/main.zig: MANAGER_SOCKET =
 * "/net/manager"). The tcp:// host below is a demo/dev-only constant. */
#ifdef CONFIG_RP23XX_INFINEON_CYW43439
#define SHERIFF_MANAGER_ADDRESS "tcp://192.168.1.1:8080"
#else
#define SHERIFF_MANAGER_ADDRESS "serial://" CDCACM_DEVPATH
#endif

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
/* Joins Wi-Fi before Sheriff's manager fetch loop starts (SHERIFF_MANAGER_
 * ADDRESS is a tcp:// socket, unreachable until associated). Credentials are
 * read live from the console; never baked into firmware/committed config.
 *
 * Associates directly via the NuttX WAPI library (wpa_driver_wext_associate/
 * netlib_obtain_ipv4addr) - the same calls platform/nuttx/vfs/vfs-wifi.c
 * makes for a wapp's /dev/wifi. */
#include <netutils/netlib.h>
#include <nuttx/wireless/wireless.h>
#include <wireless/wapi.h>

#define WIFI_IFNAME "wlan0"
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 63

/* Read one line, retrying once on an empty result: a "\r\n"-terminated send
 * leaves a bare trailing '\n' in the cooked-mode input queue, which the next
 * prompt's fgets() consumes immediately as an empty line before the real
 * answer arrives. One retry skips exactly that stray line. */
static void read_console_line(const char *prompt, char *buf, size_t bufSz) {
    for (int attempt = 0; attempt < 2; attempt++) {
        printf("%s", prompt);
        fflush(stdout);
        if (fgets(buf, (int)bufSz, stdin) == NULL) {
            buf[0] = '\0';
            return;
        }
        size_t n = strlen(buf);
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
            buf[--n] = '\0';
        if (buf[0] != '\0')
            return;
    }
}

static void wifi_connect_bringup(void) {
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1] = {0};

    read_console_line("wifi ssid: ", ssid, sizeof(ssid));
    if (ssid[0] == '\0') {
        DEBUG_TRACE("wifi_connect_bringup: no ssid entered, skipping");
        return;
    }
    read_console_line("wifi passphrase: ", pass, sizeof(pass));

    struct wpa_wconfig_s conf;
    memset(&conf, 0, sizeof(conf));
    conf.ifname = WIFI_IFNAME;
    conf.sta_mode = WAPI_MODE_MANAGED;
    conf.ssid = ssid;
    conf.ssidlen = strlen(ssid);
    conf.passphrase = pass;
    conf.phraselen = strlen(pass);
    conf.bssid = NULL;
    if (pass[0] == '\0') {
        conf.auth_wpa = IW_AUTH_WPA_VERSION_DISABLED;
        conf.cipher_mode = IW_AUTH_CIPHER_NONE;
        conf.alg = WPA_ALG_NONE;
    } else {
        conf.auth_wpa = IW_AUTH_WPA_VERSION_WPA2;
        conf.cipher_mode = IW_AUTH_CIPHER_CCMP;
        conf.alg = WPA_ALG_CCMP;
    }

    netlib_ifup(WIFI_IFNAME);
    int ret = wpa_driver_wext_associate(&conf);
    printf("wifi: associate -> %d\n", ret);
    if (ret == 0) {
        struct in_addr ip;
        /* dhcpc_open() returns EINVAL this early in boot, before the
         * network stack has settled; a short delay avoids it. */
        sleep(2);
        for (int attempt = 0; attempt < 5; attempt++) {
            int dhcpRet = netlib_obtain_ipv4addr(WIFI_IFNAME);
            memset(&ip, 0, sizeof(ip));
            netlib_get_ipv4addr(WIFI_IFNAME, &ip);
            printf("wifi: dhcp attempt %d -> %d, ip -> %s\n", attempt, dhcpRet,
                   inet_ntoa(ip));
            if (dhcpRet == 0 && ip.s_addr != 0)
                break;
        }
    }

    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
    memset(&conf, 0, sizeof(conf));
}
#endif /* CONFIG_RP23XX_INFINEON_CYW43439 */

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
    "\"" SHERIFF_MANAGER_ADDRESS "\"}]}}}"

/* Connects the CDCACM class driver so CDCACM_DEVPATH exists and is openable,
 * for the console transport or for a wapp's `serial://` socket. NuttX has
 * no "already connected" query, so this is only ever called once. */
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
    /* Must run before anything else (littlefs/ROMFS mount, seed_registry
     * file I/O) touches the shared heap and fragments its one big PSRAM
     * free node - see PlatformExtramEarlyInit's doc comment. */
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
    /* UART0 already owns fd 0-2; still connect CDC-ACM for a wapp's
     * `serial://` socket. */
    connect_usb_cdcacm();
#else
    bring_up_usb_console();
#endif

    /* Board bring-up already mounted REGISTRY_VOLUME. Done after the console
     * is up so failures here are visible instead of silently lost. */
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

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
    if (sheriffDemo) {
        wifi_connect_bringup();
    }
#endif

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
