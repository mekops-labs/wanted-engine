/* SPDX-License-Identifier: Apache-2.0 */

/* NuttX platform memory stats.
 *
 * NuttX exposes mallinfo() (not the glibc-only mallinfo2()); its uordblks/arena
 * fields are int-width, which is sufficient on 32-bit targets (heap < 2 GB). */

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __NuttX__
#include <nuttx/config.h>
#endif

#include <board-ota.h>
#include <platform.h>

void PlatformMemoryStats(size_t *heap_used, size_t *heap_total) {
    struct mallinfo mi = mallinfo();
    if (heap_used) {
        *heap_used = (size_t)mi.uordblks;
    }
    if (heap_total) {
        *heap_total = (size_t)mi.arena;
    }
}

const char *PlatformName(void) { return "nuttx"; }

/* No build-time image digest on this target. */
int PlatformFirmwareDigest(char *buf, size_t bufLen) {
    return BoardImageDigestRunning(buf, bufLen);
}

#ifdef CONFIG_ARCH_CHIP_RP23XX

/* OTP rows through the ECC-corrected read window: each row is 16 bits, packed
 * two to a word, so the stride is 2 bytes. Declared here rather than included:
 * the chip header is off the app include path. The guarded aliases bus-fault
 * on an uncorrectable row, so this window is the one a read can recover from.
 * The raw window at 0x40134000 spaces the same rows 4 bytes apart and carries
 * their ECC bits above the data, uncorrected. */
#define OTP_DATA_BASE 0x40130000u
#define OTP_ROW(n) (*(volatile const uint16_t *)(OTP_DATA_BASE + (n) * 2u))

/* Rows holding the factory-programmed 64-bit device id, least significant
 * first. It is not the Wi-Fi MAC: a board with no radio still carries one. */
#define OTP_ROW_CHIPID0 0x00u
#define OTP_CHIPID_ROWS 4

int PlatformSerialNumber(char *buf, size_t bufLen) {
    if (buf == NULL)
        return -EINVAL;
    if (bufLen < 17)
        return -ENOSPC;

    uint64_t id = 0;
    for (int i = 0; i < OTP_CHIPID_ROWS; i++) {
        id |= (uint64_t)OTP_ROW(OTP_ROW_CHIPID0 + i) << (16 * i);
    }
    /* An unprogrammed OTP reads all-zero; that is absence, not an identity. */
    if (id == 0)
        return -ENOSYS;

    int w = snprintf(buf, bufLen, "%08lx%08lx", (unsigned long)(id >> 32),
                     (unsigned long)(id & 0xffffffffu));
    if (w < 0)
        return -EIO;
    return w < (int)bufLen ? w : -ENOSPC;
}

#else /* !CONFIG_ARCH_CHIP_RP23XX */

/* NOLINTNEXTLINE(readability-non-const-parameter) */
int PlatformSerialNumber(char *buf, size_t bufLen) {
    (void)buf;
    (void)bufLen;
    return -ENOSYS;
}

#endif /* CONFIG_ARCH_CHIP_RP23XX */
