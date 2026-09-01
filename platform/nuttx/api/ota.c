/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX A/B firmware update. On RP2350 the backing is the BootROM's own
 * partition table; every other NuttX target has no A/B backing and reports a
 * single confirmed slot. See docs/platform-guide for the flash map. */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#include <board-ota.h>
#include <platform.h>

#ifdef CONFIG_ARCH_CHIP_RP23XX

#include <debug_trace.h>
#include <nuttx/fs/ioctl.h>
#include <nuttx/mtd/mtd.h>
#include <sys/ioctl.h>

/* The update slot, published by the board over the image slot the running
 * firmware did not boot from. */
#define OTA_SLOT_DEVPATH "/dev/otaslot"

/* Smallest unit the backing can program; a short tail is padded to it. */
#define OTA_PROGRAM_UNIT 256

/* Confirms the running image on the backing. Declared here rather than
 * included: the board header defining it is off the app include path, so
 * this value is part of the device's contract and must match it. */
#define OTA_IOC_CONFIRM _MTDIOC(0x00f0)

/* Boots the update slot as a flash-update boot, so the loader prefers it
 * and treats the boot as provisional. Does not return on success. */
#define OTA_IOC_BOOT_SLOT _MTDIOC(0x00f1)

/* A picobin block opens with this marker; a staged image carries one in its
 * first erase block. Enough to reject an empty or truncated slot, while the
 * loader's own hash check remains the real gate at boot. */
#define PICOBIN_BLOCK_MARKER_START 0xffffded3U
#define OTA_HEADER_SCAN_BYTES 4096

/* Items are a type byte, a size in words, then payload. The last item of a
 * block carries the offset of the next, relative to the block's own start. */
#define PICOBIN_ITEM_LAST 0xffU
#define PICOBIN_ITEM_HASH_VALUE 0x4bU
#define PICOBIN_MAX_ITEMS 32
#define PICOBIN_MAX_BLOCKS 4

/* The sealed image's SHA-256, which the seam reports as lowercase hex. */
#define IMAGE_DIGEST_BYTES (FIRMWARE_DIGEST_HEX_LEN / 2)

/* BootROM function table lookup, replicated rather than included: the chip's
 * rom header lives under arch/arm/src and is off the app include path. */
#define BOOTROM_TABLE_LOOKUP_OFFSET 0x16
#define ROM_TABLE_CODE(c1, c2) ((c1) | ((c2) << 8))
#define ROM_FUNC_GET_PARTITION_TABLE_INFO ROM_TABLE_CODE('G', 'P')
#define ROM_FUNC_FLASH_RUNTIME_TO_STORAGE_ADDR ROM_TABLE_CODE('F', 'A')
#define ROM_FUNC_GET_SYS_INFO ROM_TABLE_CODE('G', 'S')
#define RT_FLAG_FUNC_ARM_SEC 0x0004
#define RT_FLAG_FUNC_ARM_NONSEC 0x0010

/* Selects each partition's location and flags words, in partition order. */
#define PT_INFO_PARTITION_LOCATION_AND_FLAGS 0x0010

/* A partition's bounds are sector indices packed into the location word. */
#define PT_LOCATION_FIRST_SECTOR_MASK 0x00001fffU
#define PT_LOCATION_LAST_SECTOR_LSB 13
#define PT_LOCATION_LAST_SECTOR_MASK 0x00001fffU
#define PT_SECTOR_SIZE 4096U

/* Runtime base of XIP flash; the ROM translates it to the booted slot. */
#define XIP_RUNTIME_BASE 0x10000000U

/* Boot info is an echoed flags word then boot_word, diagnostic and two
 * reboot params. The try-before-you-buy flags are boot_word's top byte. */
#define SYS_INFO_BOOT_INFO 0x0040
#define BOOT_INFO_WORDS 5
#define BOOT_INFO_TBYB_SHIFT 24
#define BOOT_TBYB_BUY_PENDING 0x01U

/* Each partition contributes a location word then a flags word, after one
 * echoed flags word. */
#define PT_WORDS_PER_PARTITION 2
#define PT_QUERY_WORDS 16

/* BootROM lock protocol, replicated for the same reason the table lookup is.
 *
 * The hashing ROM APIs do not take the SHA-256 lock themselves; they only
 * check it, and the check passes while nobody holds the enable lock. Holding
 * it is therefore the caller's job, and it is what keeps a ROM call off the
 * SHA-256 hardware while something else is driving it — nothing in this build
 * does today, but the peripheral is shared and enabling a hardware digest
 * backend would make it contended with no failure that points back here.
 *
 * The enable lock is deliberately not taken: it makes the ROM reject every
 * unlocked call across all its APIs, so opting in needs an audit of every ROM
 * call site rather than only the hashing ones.
 *
 * BOOTLOCKs are hardware spinlocks in BOOTRAM. A read yields nonzero to
 * exactly one claimant; writing zero releases. */
#define BOOTRAM_BOOTLOCK(n) (0x400e0000U + 0x80cU + ((uint32_t)(n) * 4U))
#define BOOTROM_LOCK_SHA_256 0

static bool romLockAcquire(int lockNum) {
    return *(volatile uint32_t *)(uintptr_t)BOOTRAM_BOOTLOCK(lockNum) != 0;
}

static void romLockRelease(int lockNum) {
    *(volatile uint32_t *)(uintptr_t)BOOTRAM_BOOTLOCK(lockNum) = 0;
}

typedef void *(*rom_table_lookup_fn)(uint32_t code, uint32_t mask);
typedef int (*rom_get_partition_table_info_fn)(uint32_t *out, uint32_t words,
                                               uint32_t partitionAndFlags);
typedef intptr_t (*rom_flash_runtime_to_storage_addr_fn)(uintptr_t addr);
typedef int (*rom_get_sys_info_fn)(uint32_t *out, uint32_t words,
                                   uint32_t flags);

static struct {
    bool inited;
    char activeSlot;
    int slotCount;
    bool confirmed;
    /* Streaming write session, open between BeginWrite and Abort/Commit. */
    int fd;
    bool writing;
    bool pendingSwap;
    char pendingDigest[FIRMWARE_DIGEST_HEX_LEN + 1];
    size_t buffered;
    uint8_t unit[OTA_PROGRAM_UNIT];
} g_ota;

/* The engine runs secure on this board; fall back to the non-secure table so a
 * lookup never silently yields NULL if that changes. */
static void *romFuncLookup(uint32_t code) {
    rom_table_lookup_fn lookup = (rom_table_lookup_fn)(uintptr_t)*(
        uint16_t *)(uintptr_t)BOOTROM_TABLE_LOOKUP_OFFSET;
    void *fn = lookup(code, RT_FLAG_FUNC_ARM_SEC);

    if (fn == NULL)
        fn = lookup(code, RT_FLAG_FUNC_ARM_NONSEC);
    return fn;
}

/* Storage offset the running image was loaded from, or negative on error. */
static intptr_t runningStorageOffset(void) {
    rom_flash_runtime_to_storage_addr_fn toStorage =
        (rom_flash_runtime_to_storage_addr_fn)romFuncLookup(
            ROM_FUNC_FLASH_RUNTIME_TO_STORAGE_ADDR);

    if (toStorage == NULL)
        return -ENOSYS;
    return toStorage((uintptr_t)XIP_RUNTIME_BASE);
}

/* Whether this boot has no buy pending, i.e. nothing to roll back. A boot
 * state the ROM will not report is treated as confirmed: it is not evidence
 * of a pending buy, and claiming one arms a revert against the other slot. */
static bool bootConfirmed(void) {
    uint32_t words[BOOT_INFO_WORDS];
    rom_get_sys_info_fn sysInfo =
        (rom_get_sys_info_fn)romFuncLookup(ROM_FUNC_GET_SYS_INFO);

    if (sysInfo == NULL) {
        DEBUG_TRACE("ota: no sys-info entry point; reporting confirmed");
        return true;
    }

    /* Unlocked deliberately: the ROM's sys-info API does not hash, so it
     * neither uses the SHA-256 hardware nor checks the lock. Taking one here
     * would only add a way for a call that cannot fail to fail. */
    int filled = sysInfo(words, BOOT_INFO_WORDS, SYS_INFO_BOOT_INFO);
    if (filled != BOOT_INFO_WORDS || words[0] != SYS_INFO_BOOT_INFO) {
        DEBUG_TRACE("ota: boot info unavailable (%d); reporting confirmed",
                    filled);
        return true;
    }

    uint32_t tbyb = (words[1] >> BOOT_INFO_TBYB_SHIFT) & 0xffU;
    return (tbyb & BOOT_TBYB_BUY_PENDING) == 0;
}

/* Resolve which partition holds `offset`, as a slot letter. '\0' if none does,
 * which is how an image booted with no partition table reports. */
static char slotForOffset(intptr_t offset, int *countOut) {
    uint32_t words[PT_QUERY_WORDS];
    rom_get_partition_table_info_fn info =
        (rom_get_partition_table_info_fn)romFuncLookup(
            ROM_FUNC_GET_PARTITION_TABLE_INFO);

    if (info == NULL || offset < 0)
        return '\0';

    if (!romLockAcquire(BOOTROM_LOCK_SHA_256)) {
        DEBUG_TRACE("ota: sha-256 lock busy; no slot resolved");
        return '\0';
    }

    int filled =
        info(words, PT_QUERY_WORDS, PT_INFO_PARTITION_LOCATION_AND_FLAGS);
    romLockRelease(BOOTROM_LOCK_SHA_256);

    if (filled < 2 || words[0] != PT_INFO_PARTITION_LOCATION_AND_FLAGS)
        return '\0';

    int count = (filled - 1) / PT_WORDS_PER_PARTITION;
    if (countOut != NULL)
        *countOut = count;

    /* The ROM answers a flash address; partition bounds are bare offsets. */
    uint32_t bare = (uint32_t)offset;
    if (bare >= XIP_RUNTIME_BASE)
        bare -= XIP_RUNTIME_BASE;

    for (int i = 0; i < count; i++) {
        uint32_t loc = words[1 + (i * PT_WORDS_PER_PARTITION)];
        uint32_t first = loc & PT_LOCATION_FIRST_SECTOR_MASK;
        uint32_t last =
            (loc >> PT_LOCATION_LAST_SECTOR_LSB) & PT_LOCATION_LAST_SECTOR_MASK;
        uint32_t start = first * PT_SECTOR_SIZE;
        uint32_t end = (last + 1) * PT_SECTOR_SIZE;

        if (bare >= start && bare < end)
            return (char)('a' + i);
    }
    return '\0';
}

int PlatformOtaInit(void) {
    intptr_t offset = runningStorageOffset();

    /* Zeroed statics would name stdin as the session descriptor. */
    g_ota.fd = -1;

    g_ota.slotCount = 0;
    g_ota.activeSlot = slotForOffset(offset, &g_ota.slotCount);
    g_ota.confirmed = bootConfirmed();
    g_ota.inited = true;

    if (g_ota.activeSlot == '\0') {
        DEBUG_TRACE("ota: no partition slot holds storage offset %ld",
                    (long)offset);
        return 0;
    }
    DEBUG_TRACE("ota: booted slot %c of %d, storage offset 0x%lx",
                g_ota.activeSlot, g_ota.slotCount, (long)offset);
    return 0;
}

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;
    if (!g_ota.inited)
        return -EPERM;

    /* An image with no partition table behind it still runs; report it as the
     * first slot so the wire text stays well-defined. */
    out->active_slot = g_ota.activeSlot != '\0' ? g_ota.activeSlot : 'a';
    out->confirmed = g_ota.confirmed;
    out->pending_swap = g_ota.pendingSwap;

    /* An unconfirmed slot is on its first attempt: the loader hands out the
     * state of the boot it just did, not a running tally. */
    out->boot_attempts = g_ota.confirmed ? 0 : 1;

    /* Empty on purpose: the loader reports diagnostics for the boot it just
     * did rather than a history, so a truthful value needs a record of our
     * own. Reporting a slot here would be inventing one. */
    out->last_failed_slot = '\0';

    /* The staged image's own hash, taken from its block chain at commit. It
     * is what a control plane confirms a boot against -- the same kind of
     * hash PlatformFirmwareDigest reports for the running image, so the two
     * are comparable. Empty until a commit fills it. */
    strncpy(out->pending_digest, g_ota.pendingDigest,
            sizeof(out->pending_digest) - 1);
    out->pending_digest[sizeof(out->pending_digest) - 1] = '\0';
    return 0;
}

/* Hand one whole program unit to the backing, which takes nothing less. */
static int writeUnit(const uint8_t *unit) {
    ssize_t n = write(g_ota.fd, unit, OTA_PROGRAM_UNIT);

    if (n < 0)
        return -errno;
    if (n != (ssize_t)OTA_PROGRAM_UNIT)
        return -EIO;
    return 0;
}

static void endSession(void) {
    if (g_ota.fd >= 0)
        close(g_ota.fd);
    g_ota.fd = -1;
    g_ota.writing = false;
    g_ota.buffered = 0;
}

int PlatformOtaBeginWrite(void) {
    if (!g_ota.inited)
        return -EPERM;
    if (g_ota.writing)
        return -EBUSY;

    int fd = open(OTA_SLOT_DEVPATH, O_RDWR);
    if (fd < 0)
        return -errno;

    if (ioctl(fd, MTDIOC_BULKERASE, 0) < 0) {
        int err = -errno;
        close(fd);
        return err;
    }

    g_ota.fd = fd;
    g_ota.writing = true;
    g_ota.buffered = 0;
    return 0;
}

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    if (buf == NULL)
        return -EINVAL;
    if (!g_ota.writing)
        return -EPERM;

    while (len > 0) {
        size_t room = OTA_PROGRAM_UNIT - g_ota.buffered;
        size_t take = len < room ? len : room;

        memcpy(g_ota.unit + g_ota.buffered, buf, take);
        g_ota.buffered += take;
        buf += take;
        len -= take;

        if (g_ota.buffered == OTA_PROGRAM_UNIT) {
            int rc = writeUnit(g_ota.unit);
            if (rc < 0) {
                endSession();
                return rc;
            }
            g_ota.buffered = 0;
        }
    }
    return 0;
}

int PlatformOtaAbort(void) {
    endSession();
    return 0;
}

/* Reads `len` bytes at `off` within an image, however that image is reached. */
typedef int (*imageRead_t)(void *ctx, uint32_t off, void *buf, size_t len);

static int readFromMemory(void *ctx, uint32_t off, void *buf, size_t len) {
    memcpy(buf, (const uint8_t *)ctx + off, len);
    return 0;
}

static int readFromFd(void *ctx, uint32_t off, void *buf, size_t len) {
    int fd = *(const int *)ctx;

    if (lseek(fd, (off_t)off, SEEK_SET) < 0)
        return -errno;
    if (read(fd, buf, len) != (ssize_t)len)
        return -EIO;
    return 0;
}

/* The image's own SHA-256, taken from the hash item the seal writes into the
 * block chain. Walks the chain rather than scanning: the hash sits in a block
 * near the end of the image, linked from the one at the front. */
static int imageDigest(imageRead_t rd, void *ctx, uint8_t *out) {
    uint32_t blockOff = 0;
    bool found = false;

    for (uint32_t off = 0; off < OTA_HEADER_SCAN_BYTES; off += 4) {
        uint32_t word;
        if (rd(ctx, off, &word, sizeof(word)) < 0)
            return -EIO;
        if (word == PICOBIN_BLOCK_MARKER_START) {
            blockOff = off;
            found = true;
            break;
        }
    }
    if (!found)
        return -EBADMSG;

    for (int block = 0; block < PICOBIN_MAX_BLOCKS; block++) {
        uint32_t pos = blockOff + 4;

        for (int item = 0; item < PICOBIN_MAX_ITEMS; item++) {
            uint32_t header;
            if (rd(ctx, pos, &header, sizeof(header)) < 0)
                return -EIO;

            uint32_t type = header & 0xffU;
            uint32_t words = (header >> 8) & 0xffU;

            if (type == PICOBIN_ITEM_LAST) {
                uint32_t link;
                if (rd(ctx, pos + 4, &link, sizeof(link)) < 0)
                    return -EIO;
                if (link == 0)
                    return -ENOENT;
                blockOff += link;
                break;
            }

            if (type == PICOBIN_ITEM_HASH_VALUE &&
                words * 4 >= 4 + IMAGE_DIGEST_BYTES)
                return rd(ctx, pos + 4, out, IMAGE_DIGEST_BYTES);

            if (words == 0)
                return -EBADMSG;
            pos += words * 4;
        }
    }
    return -ENOENT;
}

/* NUL-terminated lowercase hex of `len` bytes; `out` holds 2*len+1. */
static void hexEncode(const uint8_t *in, size_t len, char *out) {
    static const char hex[] = "0123456789abcdef";

    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0f];
    }
    out[2 * len] = '\0';
}

int BoardImageDigestRunning(char *buf, size_t bufLen) {
    uint8_t digest[IMAGE_DIGEST_BYTES];
    int rc;

    if (buf == NULL)
        return -EINVAL;
    if (bufLen < FIRMWARE_DIGEST_HEX_LEN + 1)
        return -ENOSPC;

    rc = imageDigest(readFromMemory, (void *)(uintptr_t)XIP_RUNTIME_BASE,
                     digest);
    if (rc < 0)
        return rc;

    hexEncode(digest, sizeof(digest), buf);
    return FIRMWARE_DIGEST_HEX_LEN;
}

/* Reject a slot holding no recognisable image, so a truncated or empty
 * stream is caught here rather than as a failed boot. */
static int stagedImageLooksSane(int fd) {
    uint8_t head[OTA_HEADER_SCAN_BYTES];

    if (lseek(fd, 0, SEEK_SET) < 0)
        return -errno;
    if (read(fd, head, sizeof(head)) != (ssize_t)sizeof(head))
        return -EIO;

    for (size_t i = 0; i + sizeof(uint32_t) <= sizeof(head); i += 4) {
        uint32_t word;
        memcpy(&word, head + i, sizeof(word));
        if (word == PICOBIN_BLOCK_MARKER_START)
            return 0;
    }
    return -EBADMSG;
}

int PlatformOtaCommit(void) {
    if (!g_ota.writing)
        return -EPERM;

    /* Pad the tail to a whole program unit; erased flash reads as 0xff. */
    if (g_ota.buffered > 0) {
        memset(g_ota.unit + g_ota.buffered, 0xff,
               OTA_PROGRAM_UNIT - g_ota.buffered);
        int rc = writeUnit(g_ota.unit);
        if (rc < 0) {
            endSession();
            return rc;
        }
        g_ota.buffered = 0;
    }

    int rc = stagedImageLooksSane(g_ota.fd);
    if (rc == 0) {
        uint8_t digest[IMAGE_DIGEST_BYTES];

        /* A staged image with no readable hash still boots; the control
         * plane just cannot confirm which bytes took. */
        if (imageDigest(readFromFd, &g_ota.fd, digest) == 0)
            hexEncode(digest, sizeof(digest), g_ota.pendingDigest);
        else
            g_ota.pendingDigest[0] = '\0';
    }
    endSession();
    if (rc < 0)
        return rc;

    g_ota.pendingSwap = true;
    return 0;
}

int PlatformOtaRollback(void) {
    int fd = open(OTA_SLOT_DEVPATH, O_RDWR);
    if (fd < 0)
        return -errno;

    int rc = ioctl(fd, OTA_IOC_BOOT_SLOT, 0) < 0 ? -errno : 0;
    close(fd);
    return rc;
}

void BoardOtaBootPending(void) {
    if (!g_ota.pendingSwap)
        return;

    int fd = open(OTA_SLOT_DEVPATH, O_RDWR);
    if (fd < 0)
        return;

    (void)ioctl(fd, OTA_IOC_BOOT_SLOT, 0);
    close(fd);
}

#else /* !CONFIG_ARCH_CHIP_RP23XX */

int PlatformOtaInit(void) { return 0; }

int PlatformOtaGetBootState(platform_ota_state_t *out) {
    if (out == NULL)
        return -EINVAL;

    out->active_slot = 'a';
    out->confirmed = true;
    out->pending_swap = false;
    out->last_failed_slot = '\0';
    out->boot_attempts = 0;
    out->pending_digest[0] = '\0';
    return 0;
}

int PlatformOtaBeginWrite(void) { return -ENOSYS; }

int PlatformOtaWrite(const uint8_t *buf, size_t len) {
    (void)buf;
    (void)len;
    return -ENOSYS;
}

int PlatformOtaAbort(void) { return -ENOSYS; }

int PlatformOtaCommit(void) { return -ENOSYS; }

int PlatformOtaRollback(void) { return -ENOSYS; }

void BoardOtaBootPending(void) {}

int BoardImageDigestRunning(char *buf, size_t bufLen) {
    (void)buf;
    (void)bufLen;
    return -ENOSYS;
}

#endif /* CONFIG_ARCH_CHIP_RP23XX */

#ifdef CONFIG_ARCH_CHIP_RP23XX

int PlatformOtaConfirm(void) {
    int fd = open(OTA_SLOT_DEVPATH, O_RDWR);
    if (fd < 0)
        return -errno;

    int rc = ioctl(fd, OTA_IOC_CONFIRM, 0) < 0 ? -errno : 0;
    close(fd);
    return rc;
}

#else

int PlatformOtaConfirm(void) { return 0; }

#endif
