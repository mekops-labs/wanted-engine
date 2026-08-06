/* SPDX-License-Identifier: Apache-2.0 */

/* Host A/B backing: two slot directories, a pointer file, and a trial-boot
 * file the bootloader reads. Layout and limits: docs/platform-guide.md. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <platform.h>
#include <wanted-autoconf.h>

#define OTA_PATH_MAX 256
#define OTA_LINE_MAX 64

/* What a bootloader config include expects to find. */
static const char PREFIX_KEY[] = "os_prefix=slot_";

static int g_fd = -1; /* open staging file, -1 when no session */

static int rootPath(char *buf, size_t len, const char *leaf) {
    int n = snprintf(buf, len, "%s/%s", CONFIG_WANTED_OTA_SLOT_ROOT, leaf);
    return (n < 0 || (size_t)n >= len) ? -ENAMETOOLONG : 0;
}

static int imagePath(char *buf, size_t len, char slot, const char *suffix) {
    int n = snprintf(buf, len, "%s/slot_%c/%s%s", CONFIG_WANTED_OTA_SLOT_ROOT,
                     slot, CONFIG_WANTED_OTA_IMAGE_NAME, suffix);
    return (n < 0 || (size_t)n >= len) ? -ENAMETOOLONG : 0;
}

/* Slot named by `file`, or 0 when it is absent or names none. */
static char readSlotFile(const char *file) {
    char path[OTA_PATH_MAX];
    if (rootPath(path, sizeof(path), file) < 0)
        return 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    char line[OTA_LINE_MAX];
    ssize_t n = read(fd, line, sizeof(line) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    line[n] = '\0';

    const char *at = strstr(line, PREFIX_KEY);
    if (at == NULL)
        return 0;
    char slot = at[sizeof(PREFIX_KEY) - 1];
    return (slot == 'a' || slot == 'b') ? slot : 0;
}

static int writeSlotFile(const char *file, char slot) {
    char path[OTA_PATH_MAX];
    int rc = rootPath(path, sizeof(path), file);
    if (rc < 0)
        return rc;

    char line[OTA_LINE_MAX];
    int n = snprintf(line, sizeof(line), "%s%c/\n", PREFIX_KEY, slot);
    if (n < 0 || (size_t)n >= sizeof(line))
        return -ENAMETOOLONG;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -errno;
    rc = (write(fd, line, (size_t)n) == n) ? 0 : -EIO;
    if (rc == 0 && fsync(fd) < 0)
        rc = -errno;
    close(fd);
    return rc;
}

static int removeSlotFile(const char *file) {
    char path[OTA_PATH_MAX];
    int rc = rootPath(path, sizeof(path), file);
    if (rc < 0)
        return rc;
    if (unlink(path) < 0 && errno != ENOENT)
        return -errno;
    return 0;
}

static char activeSlot(void) {
    char slot = readSlotFile("active_slot.txt");
    return slot != 0 ? slot : 'a';
}

static char inactiveSlot(void) { return activeSlot() == 'a' ? 'b' : 'a'; }

int PlatformOtaInit(void) { return 0; }

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;

    bool pending = readSlotFile("tryboot.txt") != 0;
    out->active_slot = activeSlot();
    out->pending_swap = pending;
    out->confirmed = !pending;
    /* No host bootloader reports a failed trial back to us. */
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    /* No build-time image digest on this target, matching
     * PlatformFirmwareDigest -- a staged image is identified by version. */
    out->pending_digest[0] = '\0';
    return 0;
}

int PlatformOtaBeginWrite(void) {
    if (g_fd >= 0)
        return -EBUSY;

    char dir[OTA_PATH_MAX];
    int n = snprintf(dir, sizeof(dir), "%s/slot_%c",
                     CONFIG_WANTED_OTA_SLOT_ROOT, inactiveSlot());
    if (n < 0 || (size_t)n >= sizeof(dir))
        return -ENAMETOOLONG;
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -errno;

    char path[OTA_PATH_MAX];
    int rc = imagePath(path, sizeof(path), inactiveSlot(), ".staging");
    if (rc < 0)
        return rc;

    g_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return (g_fd < 0) ? -errno : 0;
}

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    if (g_fd < 0)
        return -EPERM;
    if (buf == NULL)
        return -EINVAL;

    size_t done = 0;
    while (done < len) {
        ssize_t n = write(g_fd, buf + done, len - done);
        if (n <= 0)
            return -errno;
        done += (size_t)n;
    }
    return 0;
}

/* Renamed into place only once whole, so a crash mid-write cannot leave a
 * short image where the bootloader looks. */
int PlatformOtaCommit(void) {
    if (g_fd < 0)
        return -EPERM;

    char slot = inactiveSlot();
    char staging[OTA_PATH_MAX];
    char image[OTA_PATH_MAX];
    int rc = imagePath(staging, sizeof(staging), slot, ".staging");
    if (rc == 0)
        rc = imagePath(image, sizeof(image), slot, "");
    if (rc == 0 && fsync(g_fd) < 0)
        rc = -errno;

    close(g_fd);
    g_fd = -1;
    if (rc < 0)
        return rc;

    if (rename(staging, image) < 0)
        return -errno;
    /* Arms the trial boot. The slot stays inactive until Confirm. */
    return writeSlotFile("tryboot.txt", slot);
}

int PlatformOtaAbort(void) {
    if (g_fd < 0)
        return 0;

    char staging[OTA_PATH_MAX];
    int rc = imagePath(staging, sizeof(staging), inactiveSlot(), ".staging");
    close(g_fd);
    g_fd = -1;
    if (rc < 0)
        return rc;
    if (unlink(staging) < 0 && errno != ENOENT)
        return -errno;
    return 0;
}

int PlatformOtaConfirm(void) {
    char pending = readSlotFile("tryboot.txt");
    if (pending == 0)
        return 0;

    int rc = writeSlotFile("active_slot.txt", pending);
    if (rc < 0)
        return rc;
    return removeSlotFile("tryboot.txt");
}

int PlatformOtaRollback(void) { return removeSlotFile("tryboot.txt"); }
