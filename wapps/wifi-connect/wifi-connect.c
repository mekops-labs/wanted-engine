/* SPDX-License-Identifier: Apache-2.0 */

/* wifi-connect — brings the board onto a WiFi network through the engine's
 * wifi device node.
 *
 * Its launch config grants the `wifi` driver, mounted at /dev/wifi, and a
 * config file at /cfg holding two lines: the SSID and the passphrase. The wapp
 * scans, logs the visible APs, associates, then polls status until connected.
 * It touches the radio only through the VFS, with no WiFi-specific ABI. */

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define WIFI_PATH    "/dev/wifi"
#define CONFIG_PATH  "/cfg/wifi.conf"
#define CONNECT_TRIES 10

/* Read up to two lines (ssid, passphrase) from the mounted config file. */
static int read_config(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz) {
    int fd = open(CONFIG_PATH, O_RDONLY);
    if (fd < 0)
        return -1;

    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl == NULL)
        return -1;
    *nl = '\0';
    strncpy(ssid, buf, ssid_sz - 1);
    ssid[ssid_sz - 1] = '\0';

    char *p = nl + 1;
    char *nl2 = strchr(p, '\n');
    if (nl2 != NULL)
        *nl2 = '\0';
    strncpy(pass, p, pass_sz - 1);
    pass[pass_sz - 1] = '\0';
    return 0;
}

int main(void) {
    int fd = open(WIFI_PATH, O_RDWR);
    if (fd < 0) {
        printf("wifi-connect: cannot open %s\n", WIFI_PATH);
        return 1;
    }

    /* Scan and log the visible networks. */
    if (write(fd, "scan", 4) < 0) {
        printf("wifi-connect: scan failed\n");
        close(fd);
        return 1;
    }
    printf("wifi-connect: scan results:\n");
    char line[128];
    ssize_t r;
    while ((r = read(fd, line, sizeof(line) - 1)) > 0) {
        line[r] = '\0';
        printf("%s", line);
    }

    /* Without credentials the wapp is a scan probe: report and exit cleanly. */
    char ssid[33] = {0};
    char pass[65] = {0};
    if (read_config(ssid, sizeof(ssid), pass, sizeof(pass)) < 0) {
        printf("wifi-connect: no %s, scan only\n", CONFIG_PATH);
        close(fd);
        return 0;
    }

    /* Associate. */
    char cmd[160];
    int n = snprintf(cmd, sizeof(cmd), "connect %s %s", ssid, pass);
    if (n < 0 || write(fd, cmd, (size_t)n) < 0) {
        printf("wifi-connect: connect command failed\n");
        close(fd);
        return 1;
    }

    /* Poll status until connected. */
    for (int i = 0; i < CONNECT_TRIES; i++) {
        r = read(fd, line, sizeof(line) - 1);
        if (r > 0) {
            line[r] = '\0';
            if (strncmp(line, "connected", 9) == 0) {
                printf("wifi-connect: %s", line);
                close(fd);
                return 0;
            }
        }
        sleep(1);
    }

    printf("wifi-connect: not connected after %d tries\n", CONNECT_TRIES);
    close(fd);
    return 1;
}
