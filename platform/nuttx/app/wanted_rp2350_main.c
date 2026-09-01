/* SPDX-License-Identifier: Apache-2.0 */

/* RP2350 init shim. The supervisor OCI TAR is bundled as a ROMFS image at /rom;
 * board bring-up mounts the registry's LittleFS volume, so this shim chdirs
 * into it, seeds factory wapps and owns the console. */

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

#include <config-nuttx.h>
#include <platform.h>
#include <wanted.h>
#include <wanted_log.h>

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
    ROMFS_MOUNTPT "/registry" /* /rom/registry (bundled factory wapps) */
#define REGISTRY_DIR REGISTRY_ROOT
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
 * into the writable registry, skipping those already installed. A freshly
 * flashed board can then start its bundled wapps with no network. */
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

/* Provisions the supervisor's identity and a launch config carrying a
 * `platform` mount and `manager` socket. Identity is written through POSIX
 * calls, since there is no interactive way to write raw CBOR. */
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

/* Sheriff's storage root, as the shipped launch configs mount it. The
 * provisioning blob and the secret a join redeems both live here. */
#define SHERIFF_STORE_DIR "sheriff"
#define SHERIFF_PROVISION_FILE SHERIFF_STORE_DIR "/provision"
#define SHERIFF_SECRET_FILE SHERIFF_STORE_DIR "/secret"
#define SHERIFF_BLOB_MAX 512

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* One line, with the trailing newline stripped. Returns its length, or -1 at
 * end of input. Unlike read_console_line it does not retry an empty line: a
 * blank line is what ends the paste. */
static int read_raw_line(char *buf, size_t bufSz) {
    if (fgets(buf, (int)bufSz, stdin) == NULL)
        return -1;
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return (int)n;
}

/* Take a provisioning blob over the console and place it where Sheriff looks
 * for it, so a bench board can enrol without a filesystem it cannot otherwise
 * write to. Paths are relative to REGISTRY_VOLUME (chdir'd by the caller).
 *
 * Prompts only on a board that holds neither a blob nor a redeemed secret, so
 * a provisioned board boots straight through. Like the Wi-Fi prompt it blocks
 * on a console with nobody attached; an empty first line skips it. */
static void provision_sheriff_blob(void) {
    /* The redeemed secret, not the blob, is what says a board is enrolled: a
     * blob that was mistyped or has expired must be replaceable. */
    if (file_exists(SHERIFF_SECRET_FILE)) {
        DEBUG_TRACE("sheriff already enrolled, not prompting");
        return;
    }

    printf("sheriff provisioning: paste device_id=, join_token= and "
           "state_key= lines,\n"
           "  then a line reading 'end' (send 'end' now to skip)\n");
    fflush(stdout);

    char blob[SHERIFF_BLOB_MAX];
    size_t used = 0;
    char line[160];
    int len;

    /* Empty lines are ignored rather than terminating: a "\r\n"-terminated
     * send leaves a bare '\n' behind, so one arrives after every pasted line.
     * That is why the paste ends on a sentinel and not on a blank line. */
    while ((len = read_raw_line(line, sizeof(line))) >= 0) {
        if (len == 0)
            continue;
        if (strcmp(line, "end") == 0)
            break;
        if (used + (size_t)len + 1 >= sizeof(blob)) {
            printf("sheriff provisioning: blob too long, discarded\n");
            return;
        }
        memcpy(blob + used, line, (size_t)len);
        used += (size_t)len;
        blob[used++] = '\n';
    }

    if (used == 0) {
        DEBUG_TRACE("no provisioning blob entered, skipping");
        return;
    }

    mkdir(SHERIFF_STORE_DIR, 0755);
    int fd = open(SHERIFF_PROVISION_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        printf("sheriff provisioning: cannot write %s: %s\n",
               SHERIFF_PROVISION_FILE, strerror(errno));
        return;
    }
    ssize_t wrote = write(fd, blob, used);
    close(fd);
    if (wrote != (ssize_t)used) {
        printf("sheriff provisioning: short write to %s\n",
               SHERIFF_PROVISION_FILE);
        return;
    }
    printf("sheriff provisioning: %u bytes stored; it enrols on this boot\n",
           (unsigned)used);
}

/* Constraints on the shipped config: the `platform` mount's src must be under
 * REGISTRY_VOLUME, and the manager socket must be named "manager". Its address
 * depends on the radio, so picking the wrong file fails at runtime. */

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
/* Joins Wi-Fi before the supervisor's manager fetch loop starts, since that
 * socket is unreachable until associated. Credentials are read live from the
 * console and never baked into firmware or committed config. */
#include <netutils/netlib.h>
#include <nuttx/wireless/wireless.h>
#include <wireless/wapi.h>

#define WIFI_IFNAME "wlan0"
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 63

/* Association is retried because a single attempt strands the board: the
 * credentials are wiped from memory once this returns, so a transient failure
 * -- an AP still coming up, a missed beacon -- costs a power cycle and
 * re-entry by hand. Bounded rather than endless, so a board with no AP in
 * range still reaches the engine instead of blocking boot forever. */
#define WIFI_ASSOC_TRIES 5
#define WIFI_ASSOC_RETRY_S 3

/* Read one line, retrying once on an empty result: a "\r\n"-terminated send
 * leaves a bare '\n' in the cooked-mode input queue, which the next prompt
 * consumes as an empty line. One retry skips exactly that stray line. */
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

/* Credentials live on the state volume, never in the firmware image and never
 * in committed config -- the two places a shared secret must not appear. The
 * file is the device's own, alongside the supervisor's enrolment secret, and
 * anyone with the debug port can read either regardless.
 *
 * Persisting them is what makes the board survive a power cycle unattended: a
 * console prompt on every boot means a headless device needs a person in front
 * of it, which is the same failure as never retrying an association. */
#ifdef CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT
#define WIFI_CRED_PATH CONFIG_RP23XX_FLASH_MTD_MOUNTPOINT "/wifi-cred"

/* `ssid\npassphrase\n`. An open passphrase is the empty second line. */
static bool wifi_cred_read(char *ssid, size_t ssidSz, char *pass,
                           size_t passSz) {
    char buf[WIFI_SSID_MAX_LEN + WIFI_PASS_MAX_LEN + 4];
    int fd = open(WIFI_CRED_PATH, O_RDONLY);

    if (fd < 0)
        return false;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return false;
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl == NULL)
        return false;
    *nl = '\0';
    if (buf[0] == '\0' || strlen(buf) >= ssidSz)
        return false;

    char *rest = nl + 1;
    char *end = strchr(rest, '\n');
    if (end != NULL)
        *end = '\0';
    if (strlen(rest) >= passSz)
        return false;

    strncpy(ssid, buf, ssidSz - 1);
    ssid[ssidSz - 1] = '\0';
    strncpy(pass, rest, passSz - 1);
    pass[passSz - 1] = '\0';
    return true;
}

static void wifi_cred_write(const char *ssid, const char *pass) {
    char buf[WIFI_SSID_MAX_LEN + WIFI_PASS_MAX_LEN + 4];
    int n = snprintf(buf, sizeof(buf), "%s\n%s\n", ssid, pass);

    if (n < 0 || (size_t)n >= sizeof(buf))
        return;

    int fd = open(WIFI_CRED_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        DEBUG_TRACE("wifi: cannot store credentials: %s", strerror(errno));
        return;
    }
    if (write(fd, buf, (size_t)n) != n)
        DEBUG_TRACE("wifi: short write storing credentials");
    close(fd);
    memset(buf, 0, sizeof(buf));
    printf("wifi: credentials stored for next boot\n");
}
#else
static bool wifi_cred_read(char *ssid, size_t ssidSz, char *pass,
                           size_t passSz) {
    (void)ssid;
    (void)ssidSz;
    (void)pass;
    (void)passSz;
    return false;
}
static void wifi_cred_write(const char *ssid, const char *pass) {
    (void)ssid;
    (void)pass;
}
#endif

/* False when nothing was entered, which is how an operator skips the join. */
static bool wifi_prompt(char *ssid, size_t ssidSz, char *pass, size_t passSz) {
    read_console_line("wifi ssid: ", ssid, ssidSz);
    if (ssid[0] == '\0') {
        DEBUG_TRACE("wifi_connect_bringup: no ssid entered, skipping");
        return false;
    }
    read_console_line("wifi passphrase: ", pass, passSz);
    return true;
}

static int wifi_associate(const char *ssid, const char *pass) {
    struct wpa_wconfig_s conf;
    int ret = -1;

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

    for (int attempt = 0; attempt < WIFI_ASSOC_TRIES; attempt++) {
        ret = wpa_driver_wext_associate(&conf);
        printf("wifi: associate attempt %d -> %d\n", attempt, ret);
        if (ret == 0)
            break;
        if (attempt + 1 < WIFI_ASSOC_TRIES)
            sleep(WIFI_ASSOC_RETRY_S);
    }

    memset(&conf, 0, sizeof(conf));
    return ret;
}

static void wifi_dhcp(void) {
    struct in_addr ip;

    /* dhcpc_open() returns EINVAL this early in boot, before the network
     * stack has settled; a short delay avoids it. */
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

static void wifi_connect_bringup(void) {
    char ssid[WIFI_SSID_MAX_LEN + 1] = {0};
    char pass[WIFI_PASS_MAX_LEN + 1] = {0};
    bool stored = wifi_cred_read(ssid, sizeof(ssid), pass, sizeof(pass));

    if (stored)
        printf("wifi: joining \"%s\" from stored credentials\n", ssid);
    else if (!wifi_prompt(ssid, sizeof(ssid), pass, sizeof(pass)))
        return;

    int ret = wifi_associate(ssid, pass);

    /* Stored credentials that stopped working must not strand the board:
     * fall back to asking, rather than leaving it off the network for good. */
    if (ret != 0 && stored) {
        printf("wifi: stored credentials did not associate\n");
        if (!wifi_prompt(ssid, sizeof(ssid), pass, sizeof(pass)))
            goto out;
        stored = false;
        ret = wifi_associate(ssid, pass);
    }

    if (ret != 0) {
        printf("wifi: not associated after %d attempts; continuing without "
               "network\n",
               WIFI_ASSOC_TRIES);
        goto out;
    }

    /* Only credentials that actually associated are kept, so a typo is not
     * stored and then replayed on every boot. */
    if (!stored)
        wifi_cred_write(ssid, pass);

    wifi_dhcp();

out:
    memset(ssid, 0, sizeof(ssid));
    memset(pass, 0, sizeof(pass));
}
#endif /* CONFIG_RP23XX_INFINEON_CYW43439 */

/* Launch config as bytes — no filesystem holds one at first boot. Generated by
 * test/nuttx-sim.sh from the configured JSON. */
#include "wanted-config.h"

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
 * until a host terminal opens the port and sends a few carriage returns before
 * binding fd 0-2, or early engine output races the terminal attaching. */
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

/* Read the boot state and record a slot that failed. An unconfirmed active
 * slot on a repeat attempt means the loader already tried an image and came
 * back, which is the one account of a bad update that outlives the reboot
 * that hid it. Goes to the engine's error channel, which survives a reset. */
static void report_boot_slot(void) {
    if (PlatformOtaInit() != 0) {
        DEBUG_TRACE("ota: boot state unavailable");
        return;
    }

    platform_ota_state_t st;
    if (PlatformOtaGetBootState(&st) != 0)
        return;

    DEBUG_TRACE("ota: booted slot %c (%s), attempt %d", st.active_slot,
                st.confirmed ? "confirmed" : "unconfirmed", st.boot_attempts);

    if (!st.confirmed && st.boot_attempts > 1)
        LOG_ERROR("ota: slot %c did not confirm after %d attempts; "
                  "the loader reverted",
                  st.active_slot, st.boot_attempts);
}

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
            provision_sheriff_blob();
        }
    }

#ifdef CONFIG_RP23XX_INFINEON_CYW43439
    if (sheriffDemo) {
        wifi_connect_bringup();
    }
#endif

    /* Before the engine starts, so a slot that failed is recorded even if the
     * supervisor never comes up. */
    report_boot_slot();

    int rc;
    if (sheriffDemo) {
        rc = WantedStart(wantedDefaultConfig, strlen(wantedDefaultConfig));
    } else {
        rc = wanted_main(argc, argv);
    }

    /* Engine loop returned (supervisor drained / poweroff requested). Power the
     * board off so we don't idle in init; falls through if the config lacks
     * BOARDCTL_POWEROFF. */
    boardctl(BOARDIOC_POWEROFF, rc);
    return rc;
}
